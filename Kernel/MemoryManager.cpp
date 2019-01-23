#include "MemoryManager.h"
#include <AK/Assertions.h>
#include <AK/kstdio.h>
#include <AK/kmalloc.h>
#include "i386.h"
#include "StdLib.h"
#include "Process.h"
#include <LibC/errno_numbers.h>

//#define MM_DEBUG
//#define PAGE_FAULT_DEBUG

static MemoryManager* s_the;

MemoryManager& MM
{
    return *s_the;
}

MemoryManager::MemoryManager()
{
    m_kernel_page_directory = PageDirectory::create_at_fixed_address(PhysicalAddress(0x4000));
    m_page_table_zero = (dword*)0x6000;

    initialize_paging();
}

MemoryManager::~MemoryManager()
{
}

PageDirectory::PageDirectory(PhysicalAddress paddr)
{
    kprintf("Instantiating PageDirectory with specific paddr P%x\n", paddr.get());
    m_directory_page = adopt(*new PhysicalPage(paddr, true));
}

PageDirectory::PageDirectory()
{
    MM.populate_page_directory(*this);
}

void MemoryManager::populate_page_directory(PageDirectory& page_directory)
{
    page_directory.m_directory_page = allocate_supervisor_physical_page();
    memset(page_directory.entries(), 0, PAGE_SIZE);
    page_directory.entries()[0] = kernel_page_directory().entries()[0];
}

void MemoryManager::initialize_paging()
{
    static_assert(sizeof(MemoryManager::PageDirectoryEntry) == 4);
    static_assert(sizeof(MemoryManager::PageTableEntry) == 4);
    memset(m_page_table_zero, 0, PAGE_SIZE);

#ifdef MM_DEBUG
    dbgprintf("MM: Kernel page directory @ %p\n", kernel_page_directory().cr3());
#endif

#ifdef MM_DEBUG
    dbgprintf("MM: Protect against null dereferences\n");
#endif
    // Make null dereferences crash.
    map_protected(LinearAddress(0), PAGE_SIZE);

#ifdef MM_DEBUG
    dbgprintf("MM: Identity map bottom 4MB\n");
#endif
    // The bottom 4 MB (except for the null page) are identity mapped & supervisor only.
    // Every process shares these mappings.
    create_identity_mapping(kernel_page_directory(), LinearAddress(PAGE_SIZE), (4 * MB) - PAGE_SIZE);

    // Basic memory map:
    // 0      -> 512 kB         Kernel code. Root page directory & PDE 0.
    // 1 MB   -> 2 MB           kmalloc_eternal() space.
    // 2 MB   -> 3 MB           kmalloc() space.
    // 3 MB   -> 4 MB           Supervisor physical pages (available for allocation!)
    // 4 MB   -> 32 MB          Userspace physical pages (available for allocation!)
    for (size_t i = (2 * MB); i < (4 * MB); i += PAGE_SIZE)
        m_free_supervisor_physical_pages.append(adopt(*new PhysicalPage(PhysicalAddress(i), true)));

#ifdef MM_DEBUG
    dbgprintf("MM: 4MB-32MB available for allocation\n");
#endif
    for (size_t i = (4 * MB); i < (32 * MB); i += PAGE_SIZE)
        m_free_physical_pages.append(adopt(*new PhysicalPage(PhysicalAddress(i), false)));
    m_quickmap_addr = LinearAddress(m_free_physical_pages.take_last().leakRef()->paddr().get());
#ifdef MM_DEBUG
    dbgprintf("MM: Quickmap will use P%x\n", m_quickmap_addr.get());
    dbgprintf("MM: Installing page directory\n");
#endif
    asm volatile("movl %%eax, %%cr3"::"a"(kernel_page_directory().cr3()));
    asm volatile(
        "movl %cr0, %eax\n"
        "orl $0x80000001, %eax\n"
        "movl %eax, %cr0\n"
    );
}

RetainPtr<PhysicalPage> MemoryManager::allocate_page_table(PageDirectory& page_directory, unsigned index)
{
    ASSERT(!page_directory.m_physical_pages.contains(index));
    auto physical_page = allocate_supervisor_physical_page();
    if (!physical_page)
        return nullptr;
    dword address = physical_page->paddr().get();
    memset((void*)address, 0, PAGE_SIZE);
    page_directory.m_physical_pages.set(index, physical_page.copyRef());
    return physical_page;
}

void MemoryManager::remove_identity_mapping(PageDirectory& page_directory, LinearAddress laddr, size_t size)
{
    InterruptDisabler disabler;
    // FIXME: ASSERT(laddr is 4KB aligned);
    for (dword offset = 0; offset < size; offset += PAGE_SIZE) {
        auto pte_address = laddr.offset(offset);
        auto pte = ensure_pte(page_directory, pte_address);
        pte.set_physical_page_base(0);
        pte.set_user_allowed(false);
        pte.set_present(true);
        pte.set_writable(true);
        flush_tlb(pte_address);
    }
}

auto MemoryManager::ensure_pte(PageDirectory& page_directory, LinearAddress laddr) -> PageTableEntry
{
    ASSERT_INTERRUPTS_DISABLED();
    dword page_directory_index = (laddr.get() >> 22) & 0x3ff;
    dword page_table_index = (laddr.get() >> 12) & 0x3ff;

    PageDirectoryEntry pde = PageDirectoryEntry(&page_directory.entries()[page_directory_index]);
    if (!pde.is_present()) {
#ifdef MM_DEBUG
        dbgprintf("MM: PDE %u not present (requested for L%x), allocating\n", page_directory_index, laddr.get());
#endif
        if (page_directory_index == 0) {
            ASSERT(&page_directory == m_kernel_page_directory.ptr());
            pde.setPageTableBase((dword)m_page_table_zero);
            pde.set_user_allowed(false);
            pde.set_present(true);
            pde.set_writable(true);
        } else {
            ASSERT(&page_directory != m_kernel_page_directory.ptr());
            auto page_table = allocate_page_table(page_directory, page_directory_index);
#ifdef MM_DEBUG
            dbgprintf("MM: PD K%x (%s) at P%x allocated page table #%u (for L%x) at P%x\n",
                &page_directory,
                &page_directory == m_kernel_page_directory.ptr() ? "Kernel" : "User",
                page_directory.cr3(),
                page_directory_index,
                laddr.get(),
                page_table->paddr().get());
#endif

            pde.setPageTableBase(page_table->paddr().get());
            pde.set_user_allowed(true);
            pde.set_present(true);
            pde.set_writable(true);
            page_directory.m_physical_pages.set(page_directory_index, move(page_table));
        }
    }
    return PageTableEntry(&pde.pageTableBase()[page_table_index]);
}

void MemoryManager::map_protected(LinearAddress linearAddress, size_t length)
{
    InterruptDisabler disabler;
    // FIXME: ASSERT(linearAddress is 4KB aligned);
    for (dword offset = 0; offset < length; offset += PAGE_SIZE) {
        auto pteAddress = linearAddress.offset(offset);
        auto pte = ensure_pte(kernel_page_directory(), pteAddress);
        pte.set_physical_page_base(pteAddress.get());
        pte.set_user_allowed(false);
        pte.set_present(false);
        pte.set_writable(false);
        flush_tlb(pteAddress);
    }
}

void MemoryManager::create_identity_mapping(PageDirectory& page_directory, LinearAddress laddr, size_t size)
{
    InterruptDisabler disabler;
    ASSERT((laddr.get() & ~PAGE_MASK) == 0);
    for (dword offset = 0; offset < size; offset += PAGE_SIZE) {
        auto pteAddress = laddr.offset(offset);
        auto pte = ensure_pte(page_directory, pteAddress);
        pte.set_physical_page_base(pteAddress.get());
        pte.set_user_allowed(false);
        pte.set_present(true);
        pte.set_writable(true);
        page_directory.flush(pteAddress);
    }
}

void MemoryManager::initialize()
{
    s_the = new MemoryManager;
}

Region* MemoryManager::region_from_laddr(Process& process, LinearAddress laddr)
{
    ASSERT_INTERRUPTS_DISABLED();

    // FIXME: Use a binary search tree (maybe red/black?) or some other more appropriate data structure!
    for (auto& region : process.m_regions) {
        if (region->contains(laddr))
            return region.ptr();
    }
    kprintf("%s(%u) Couldn't find region for L%x (CR3=%x)\n", process.name().characters(), process.pid(), laddr.get(), process.page_directory().cr3());
    return nullptr;
}

bool MemoryManager::zero_page(Region& region, unsigned page_index_in_region)
{
    ASSERT_INTERRUPTS_DISABLED();
    auto& vmo = region.vmo();
    auto physical_page = allocate_physical_page();
    byte* dest_ptr = quickmap_page(*physical_page);
    memset(dest_ptr, 0, PAGE_SIZE);
#ifdef PAGE_FAULT_DEBUG
    dbgprintf("      >> ZERO P%x\n", physical_page->paddr().get());
#endif
    unquickmap_page();
    region.cow_map.set(page_index_in_region, false);
    vmo.physical_pages()[page_index_in_region] = move(physical_page);
    remap_region_page(region, page_index_in_region, true);
    return true;
}

bool MemoryManager::copy_on_write(Region& region, unsigned page_index_in_region)
{
    ASSERT_INTERRUPTS_DISABLED();
    auto& vmo = region.vmo();
    if (vmo.physical_pages()[page_index_in_region]->retain_count() == 1) {
#ifdef PAGE_FAULT_DEBUG
        dbgprintf("    >> It's a COW page but nobody is sharing it anymore. Remap r/w\n");
#endif
        region.cow_map.set(page_index_in_region, false);
        remap_region_page(region, page_index_in_region, true);
        return true;
    }

#ifdef PAGE_FAULT_DEBUG
    dbgprintf("    >> It's a COW page and it's time to COW!\n");
#endif
    auto physical_page_to_copy = move(vmo.physical_pages()[page_index_in_region]);
    auto physical_page = allocate_physical_page();
    byte* dest_ptr = quickmap_page(*physical_page);
    const byte* src_ptr = region.linearAddress.offset(page_index_in_region * PAGE_SIZE).asPtr();
#ifdef PAGE_FAULT_DEBUG
    dbgprintf("      >> COW P%x <- P%x\n", physical_page->paddr().get(), physical_page_to_copy->paddr().get());
#endif
    memcpy(dest_ptr, src_ptr, PAGE_SIZE);
    vmo.physical_pages()[page_index_in_region] = move(physical_page);
    unquickmap_page();
    region.cow_map.set(page_index_in_region, false);
    remap_region_page(region, page_index_in_region, true);
    return true;
}

bool Region::page_in()
{
    ASSERT(m_page_directory);
    ASSERT(!vmo().is_anonymous());
    ASSERT(vmo().inode());
#ifdef MM_DEBUG
    dbgprintf("MM: page_in %u pages\n", page_count());
#endif
    for (size_t i = 0; i < page_count(); ++i) {
        auto& vmo_page = vmo().physical_pages()[first_page_index() + i];
        if (vmo_page.is_null()) {
            bool success = MM.page_in_from_inode(*this, i);
            if (!success)
                return false;
        }
        MM.remap_region_page(*this, i, true);
    }
    return true;
}

bool MemoryManager::page_in_from_inode(Region& region, unsigned page_index_in_region)
{
    ASSERT(region.m_page_directory);
    auto& vmo = region.vmo();
    ASSERT(!vmo.is_anonymous());
    ASSERT(vmo.inode());
    auto& inode = *vmo.inode();
    auto& vmo_page = vmo.physical_pages()[region.first_page_index() + page_index_in_region];
    ASSERT(vmo_page.is_null());
    vmo_page = allocate_physical_page();
    if (vmo_page.is_null()) {
        kprintf("MM: page_in_from_inode was unable to allocate a physical page\n");
        return false;
    }
    remap_region_page(region, page_index_in_region, true);
    byte* dest_ptr = region.linearAddress.offset(page_index_in_region * PAGE_SIZE).asPtr();
#ifdef MM_DEBUG
    dbgprintf("MM: page_in_from_inode ready to read from inode, will write to L%x!\n", dest_ptr);
#endif
    sti(); // Oh god here we go...
    auto nread = inode.read_bytes(vmo.inode_offset() + ((region.first_page_index() + page_index_in_region) * PAGE_SIZE), PAGE_SIZE, dest_ptr, nullptr);
    if (nread < 0) {
        kprintf("MM: page_in_from_inode had error (%d) while reading!\n", nread);
        return false;
    }
    if (nread < PAGE_SIZE) {
        // If we read less than a page, zero out the rest to avoid leaking uninitialized data.
        memset(dest_ptr + nread, 0, PAGE_SIZE - nread);
    }
    cli();
    return true;
}

PageFaultResponse MemoryManager::handle_page_fault(const PageFault& fault)
{
    ASSERT_INTERRUPTS_DISABLED();
#ifdef PAGE_FAULT_DEBUG
    dbgprintf("MM: handle_page_fault(%w) at L%x\n", fault.code(), fault.laddr().get());
#endif
    ASSERT(fault.laddr() != m_quickmap_addr);
    auto* region = region_from_laddr(*current, fault.laddr());
    if (!region) {
        kprintf("NP(error) fault at invalid address L%x\n", fault.laddr().get());
        return PageFaultResponse::ShouldCrash;
    }
    auto page_index_in_region = region->page_index_from_address(fault.laddr());
    if (fault.is_not_present()) {
        if (region->vmo().inode()) {
            dbgprintf("NP(inode) fault in Region{%p}[%u]\n", region, page_index_in_region);
            page_in_from_inode(*region, page_index_in_region);
            return PageFaultResponse::Continue;
        } else {
            dbgprintf("NP(zero) fault in Region{%p}[%u]\n", region, page_index_in_region);
            zero_page(*region, page_index_in_region);
            return PageFaultResponse::Continue;
        }
    } else if (fault.is_protection_violation()) {
        if (region->cow_map.get(page_index_in_region)) {
            dbgprintf("PV(cow) fault in Region{%p}[%u]\n", region, page_index_in_region);
            bool success = copy_on_write(*region, page_index_in_region);
            ASSERT(success);
            return PageFaultResponse::Continue;
        }
        kprintf("PV(error) fault in Region{%p}[%u]\n", region, page_index_in_region);
    } else {
        ASSERT_NOT_REACHED();
    }

    return PageFaultResponse::ShouldCrash;
}

RetainPtr<PhysicalPage> MemoryManager::allocate_physical_page()
{
    InterruptDisabler disabler;
    if (1 > m_free_physical_pages.size())
        return { };
#ifdef MM_DEBUG
    dbgprintf("MM: allocate_physical_page vending P%x (%u remaining)\n", m_free_physical_pages.last()->paddr().get(), m_free_physical_pages.size());
#endif
    return m_free_physical_pages.take_last();
}

RetainPtr<PhysicalPage> MemoryManager::allocate_supervisor_physical_page()
{
    InterruptDisabler disabler;
    if (1 > m_free_supervisor_physical_pages.size())
        return { };
#ifdef MM_DEBUG
    dbgprintf("MM: allocate_supervisor_physical_page vending P%x (%u remaining)\n", m_free_supervisor_physical_pages.last()->paddr().get(), m_free_supervisor_physical_pages.size());
#endif
    return m_free_supervisor_physical_pages.take_last();
}

void MemoryManager::enter_process_paging_scope(Process& process)
{
    InterruptDisabler disabler;
    current->m_tss.cr3 = process.page_directory().cr3();
    asm volatile("movl %%eax, %%cr3"::"a"(process.page_directory().cr3()):"memory");
}

void MemoryManager::flush_entire_tlb()
{
    asm volatile(
        "mov %cr3, %eax\n"
        "mov %eax, %cr3\n"
     );
}

void MemoryManager::flush_tlb(LinearAddress laddr)
{
    asm volatile("invlpg %0": :"m" (*(char*)laddr.get()) : "memory");
}

byte* MemoryManager::quickmap_page(PhysicalPage& physical_page)
{
    ASSERT_INTERRUPTS_DISABLED();
    auto page_laddr = m_quickmap_addr;
    auto pte = ensure_pte(current->page_directory(), page_laddr);
    pte.set_physical_page_base(physical_page.paddr().get());
    pte.set_present(true);
    pte.set_writable(true);
    flush_tlb(page_laddr);
    ASSERT((dword)pte.physical_page_base() == physical_page.paddr().get());
#ifdef MM_DEBUG
    dbgprintf("MM: >> quickmap_page L%x => P%x @ PTE=%p\n", page_laddr, physical_page.paddr().get(), pte.ptr());
#endif
    return page_laddr.asPtr();
}

void MemoryManager::unquickmap_page()
{
    ASSERT_INTERRUPTS_DISABLED();
    auto page_laddr = m_quickmap_addr;
    auto pte = ensure_pte(current->page_directory(), page_laddr);
#ifdef MM_DEBUG
    auto old_physical_address = pte.physical_page_base();
#endif
    pte.set_physical_page_base(0);
    pte.set_present(false);
    pte.set_writable(false);
    flush_tlb(page_laddr);
#ifdef MM_DEBUG
    dbgprintf("MM: >> unquickmap_page L%x =/> P%x\n", page_laddr, old_physical_address);
#endif
}

void MemoryManager::remap_region_page(Region& region, unsigned page_index_in_region, bool user_allowed)
{
    ASSERT(region.m_page_directory);
    InterruptDisabler disabler;
    auto page_laddr = region.linearAddress.offset(page_index_in_region * PAGE_SIZE);
    auto pte = ensure_pte(*region.m_page_directory, page_laddr);
    auto& physical_page = region.vmo().physical_pages()[page_index_in_region];
    ASSERT(physical_page);
    pte.set_physical_page_base(physical_page->paddr().get());
    pte.set_present(true); // FIXME: Maybe we should use the is_readable flag here?
    if (region.cow_map.get(page_index_in_region))
        pte.set_writable(false);
    else
        pte.set_writable(region.is_writable);
    pte.set_user_allowed(user_allowed);
    region.m_page_directory->flush(page_laddr);
#ifdef MM_DEBUG
    dbgprintf("MM: >> remap_region_page (PD=%x, PTE=P%x) '%s' L%x => P%x (@%p)\n", region.m_page_directory->cr3(), pte.ptr(), region.name.characters(), page_laddr.get(), physical_page->paddr().get(), physical_page.ptr());
#endif
}

void MemoryManager::remap_region(Process& process, Region& region)
{
    InterruptDisabler disabler;
    map_region_at_address(process.page_directory(), region, region.linearAddress, true);
}

void MemoryManager::map_region_at_address(PageDirectory& page_directory, Region& region, LinearAddress laddr, bool user_allowed)
{
    InterruptDisabler disabler;
    region.m_page_directory = page_directory;
    auto& vmo = region.vmo();
#ifdef MM_DEBUG
    dbgprintf("MM: map_region_at_address will map VMO pages %u - %u (VMO page count: %u)\n", region.first_page_index(), region.last_page_index(), vmo.page_count());
#endif
    for (size_t i = 0; i < region.page_count(); ++i) {
        auto page_laddr = laddr.offset(i * PAGE_SIZE);
        auto pte = ensure_pte(page_directory, page_laddr);
        auto& physical_page = vmo.physical_pages()[region.first_page_index() + i];
        if (physical_page) {
            pte.set_physical_page_base(physical_page->paddr().get());
            pte.set_present(true); // FIXME: Maybe we should use the is_readable flag here?
            // FIXME: It seems wrong that the *region* cow map is essentially using *VMO* relative indices.
            if (region.cow_map.get(region.first_page_index() + i))
                pte.set_writable(false);
            else
                pte.set_writable(region.is_writable);
        } else {
            pte.set_physical_page_base(0);
            pte.set_present(false);
            pte.set_writable(region.is_writable);
        }
        pte.set_user_allowed(user_allowed);
        page_directory.flush(page_laddr);
#ifdef MM_DEBUG
        dbgprintf("MM: >> map_region_at_address (PD=%x) '%s' L%x => P%x (@%p)\n", &page_directory, region.name.characters(), page_laddr, physical_page ? physical_page->paddr().get() : 0, physical_page.ptr());
#endif
    }
}

bool MemoryManager::unmap_region(Region& region)
{
    ASSERT(region.m_page_directory);
    InterruptDisabler disabler;
    for (size_t i = 0; i < region.page_count(); ++i) {
        auto laddr = region.linearAddress.offset(i * PAGE_SIZE);
        auto pte = ensure_pte(*region.m_page_directory, laddr);
        pte.set_physical_page_base(0);
        pte.set_present(false);
        pte.set_writable(false);
        pte.set_user_allowed(false);
        region.m_page_directory->flush(laddr);
#ifdef MM_DEBUG
        auto& physical_page = region.vmo().physical_pages()[region.first_page_index() + i];
        dbgprintf("MM: >> Unmapped L%x => P%x <<\n", laddr, physical_page ? physical_page->paddr().get() : 0);
#endif
    }
    region.m_page_directory.clear();
    return true;
}

bool MemoryManager::map_region(Process& process, Region& region)
{
    map_region_at_address(process.page_directory(), region, region.linearAddress, true);
    return true;
}

bool MemoryManager::validate_user_read(const Process& process, LinearAddress laddr) const
{
    dword pageDirectoryIndex = (laddr.get() >> 22) & 0x3ff;
    dword pageTableIndex = (laddr.get() >> 12) & 0x3ff;
    auto pde = PageDirectoryEntry(&const_cast<Process&>(process).page_directory().entries()[pageDirectoryIndex]);
    if (!pde.is_present())
        return false;
    auto pte = PageTableEntry(&pde.pageTableBase()[pageTableIndex]);
    if (!pte.is_present())
        return false;
    if (process.isRing3() && !pte.is_user_allowed())
        return false;
    return true;
}

bool MemoryManager::validate_user_write(const Process& process, LinearAddress laddr) const
{
    dword pageDirectoryIndex = (laddr.get() >> 22) & 0x3ff;
    dword pageTableIndex = (laddr.get() >> 12) & 0x3ff;
    auto pde = PageDirectoryEntry(&const_cast<Process&>(process).page_directory().entries()[pageDirectoryIndex]);
    if (!pde.is_present())
        return false;
    auto pte = PageTableEntry(&pde.pageTableBase()[pageTableIndex]);
    if (!pte.is_present())
        return false;
    if (process.isRing3() && !pte.is_user_allowed())
        return false;
    if (!pte.is_writable())
        return false;
    return true;
}

RetainPtr<Region> Region::clone()
{
    InterruptDisabler disabler;

    if (m_shared || (is_readable && !is_writable)) {
        // Create a new region backed by the same VMObject.
        return adopt(*new Region(linearAddress, size, m_vmo.copyRef(), m_offset_in_vmo, String(name), is_readable, is_writable));
    }

    dbgprintf("%s<%u> Region::clone(): cowing %s (L%x)\n",
              current->name().characters(),
              current->pid(),
              name.characters(),
              linearAddress.get());
    // Set up a COW region. The parent (this) region becomes COW as well!
    for (size_t i = 0; i < page_count(); ++i)
        cow_map.set(i, true);
    MM.remap_region(*current, *this);
    return adopt(*new Region(linearAddress, size, m_vmo->clone(), m_offset_in_vmo, String(name), is_readable, is_writable, true));
}

Region::Region(LinearAddress a, size_t s, String&& n, bool r, bool w, bool cow)
    : linearAddress(a)
    , size(s)
    , m_vmo(VMObject::create_anonymous(s))
    , name(move(n))
    , is_readable(r)
    , is_writable(w)
    , cow_map(Bitmap::create(m_vmo->page_count(), cow))
{
    m_vmo->set_name(name);
    MM.register_region(*this);
}

Region::Region(LinearAddress a, size_t s, RetainPtr<Inode>&& inode, String&& n, bool r, bool w)
    : linearAddress(a)
    , size(s)
    , m_vmo(VMObject::create_file_backed(move(inode), s))
    , name(move(n))
    , is_readable(r)
    , is_writable(w)
    , cow_map(Bitmap::create(m_vmo->page_count()))
{
    MM.register_region(*this);
}

Region::Region(LinearAddress a, size_t s, RetainPtr<VMObject>&& vmo, size_t offset_in_vmo, String&& n, bool r, bool w, bool cow)
    : linearAddress(a)
    , size(s)
    , m_offset_in_vmo(offset_in_vmo)
    , m_vmo(move(vmo))
    , name(move(n))
    , is_readable(r)
    , is_writable(w)
    , cow_map(Bitmap::create(m_vmo->page_count(), cow))
{
    MM.register_region(*this);
}

Region::~Region()
{
    if (m_page_directory) {
        MM.unmap_region(*this);
        ASSERT(!m_page_directory);
    }
    MM.unregister_region(*this);
}

PhysicalPage::PhysicalPage(PhysicalAddress paddr, bool supervisor)
    : m_supervisor(supervisor)
    , m_paddr(paddr)
{
}

void PhysicalPage::return_to_freelist()
{
    ASSERT((paddr().get() & ~PAGE_MASK) == 0);
    InterruptDisabler disabler;
    m_retain_count = 1;
    if (m_supervisor)
        MM.m_free_supervisor_physical_pages.append(adopt(*this));
    else
        MM.m_free_physical_pages.append(adopt(*this));
#ifdef MM_DEBUG
    dbgprintf("MM: P%x released to freelist\n", m_paddr.get());
#endif
}

RetainPtr<VMObject> VMObject::create_file_backed(RetainPtr<Inode>&& inode, size_t size)
{
    InterruptDisabler disabler;
    if (inode->vmo())
        return static_cast<VMObject*>(inode->vmo());
    size = ceilDiv(size, PAGE_SIZE) * PAGE_SIZE;
    auto vmo = adopt(*new VMObject(move(inode), size));
    vmo->inode()->set_vmo(vmo.ptr());
    return vmo;
}

RetainPtr<VMObject> VMObject::create_anonymous(size_t size)
{
    size = ceilDiv(size, PAGE_SIZE) * PAGE_SIZE;
    return adopt(*new VMObject(size));
}

RetainPtr<VMObject> VMObject::create_framebuffer_wrapper(PhysicalAddress paddr, size_t size)
{
    size = ceilDiv(size, PAGE_SIZE) * PAGE_SIZE;
    return adopt(*new VMObject(paddr, size));
}

RetainPtr<VMObject> VMObject::clone()
{
    return adopt(*new VMObject(*this));
}

VMObject::VMObject(VMObject& other)
    : m_name(other.m_name)
    , m_anonymous(other.m_anonymous)
    , m_inode_offset(other.m_inode_offset)
    , m_size(other.m_size)
    , m_inode(other.m_inode)
    , m_physical_pages(other.m_physical_pages)
{
    MM.register_vmo(*this);
}

VMObject::VMObject(size_t size)
    : m_anonymous(true)
    , m_size(size)
{
    MM.register_vmo(*this);
    m_physical_pages.resize(page_count());
}

VMObject::VMObject(PhysicalAddress paddr, size_t size)
    : m_anonymous(true)
    , m_size(size)
{
    MM.register_vmo(*this);
    for (size_t i = 0; i < size; i += PAGE_SIZE) {
        m_physical_pages.append(adopt(*new PhysicalPage(paddr.offset(i), false)));
    }
    ASSERT(m_physical_pages.size() == page_count());
}


VMObject::VMObject(RetainPtr<Inode>&& inode, size_t size)
    : m_size(size)
    , m_inode(move(inode))
{
    m_physical_pages.resize(page_count());
    MM.register_vmo(*this);
}

VMObject::~VMObject()
{
    if (m_inode) {
        ASSERT(m_inode->vmo() == this);
        m_inode->set_vmo(nullptr);
    }
    MM.unregister_vmo(*this);
}

int Region::commit()
{
    InterruptDisabler disabler;
#ifdef MM_DEBUG
    dbgprintf("MM: commit %u pages in Region %p (VMO=%p) at L%x\n", vmo().page_count(), this, &vmo(), linearAddress.get());
#endif
    for (size_t i = first_page_index(); i <= last_page_index(); ++i) {
        if (!vmo().physical_pages()[i].is_null())
            continue;
        auto physical_page = MM.allocate_physical_page();
        if (!physical_page) {
            kprintf("MM: commit was unable to allocate a physical page\n");
            return -ENOMEM;
        }
        vmo().physical_pages()[i] = move(physical_page);
        MM.remap_region_page(*this, i, true);
    }
    return 0;
}

void MemoryManager::register_vmo(VMObject& vmo)
{
    InterruptDisabler disabler;
    m_vmos.set(&vmo);
}

void MemoryManager::unregister_vmo(VMObject& vmo)
{
    InterruptDisabler disabler;
    m_vmos.remove(&vmo);
}

void MemoryManager::register_region(Region& region)
{
    InterruptDisabler disabler;
    m_regions.set(&region);
}

void MemoryManager::unregister_region(Region& region)
{
    InterruptDisabler disabler;
    m_regions.remove(&region);
}

size_t Region::committed() const
{
    size_t bytes = 0;
    for (size_t i = 0; i < page_count(); ++i) {
        if (m_vmo->physical_pages()[first_page_index() + i])
            bytes += PAGE_SIZE;
    }
    return bytes;
}

PageDirectory::~PageDirectory()
{
    ASSERT_INTERRUPTS_DISABLED();
#ifdef MM_DEBUG
    dbgprintf("MM: ~PageDirectory K%x\n", this);
#endif
}

void PageDirectory::flush(LinearAddress laddr)
{
    if (&current->page_directory() == this)
        MM.flush_tlb(laddr);
}
