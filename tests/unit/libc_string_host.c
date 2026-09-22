/* Runtime imports for a renamed copy of the real string.c. Only memrchr is
 * exercised by the libgen gate; strdup/strndup still need their allocator
 * imports at link time. Keep these separate from the host's errno and malloc
 * symbols so loading this object cannot change the host runtime's behavior. */
#include <stdlib.h>

int mini_errno;

void *mini_malloc(size_t size)
{
    return malloc(size);
}
