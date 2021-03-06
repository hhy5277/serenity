#include "TemporaryFile.h"
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

namespace AK {

TemporaryFile::TemporaryFile()
{
    char nameBuffer[] = "/tmp/AKTemporaryFile.XXXXXX";
    int fd = mkstemp(nameBuffer);
    if (fd != -1) {
        m_stream = fdopen(fd, "w+");
        m_file_name = nameBuffer;
    }
}

TemporaryFile::~TemporaryFile()
{
    if (is_valid()) {
        unlink(m_file_name.characters());
        fclose(m_stream);
    }
}

void TemporaryFile::sync()
{
    if (m_stream)
        fflush(m_stream);
}

}

