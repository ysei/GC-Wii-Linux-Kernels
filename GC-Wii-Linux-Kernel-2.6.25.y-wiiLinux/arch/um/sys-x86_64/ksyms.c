#include "linux/module.h"
#include "linux/in6.h"
#include "linux/rwsem.h"
#include "asm/byteorder.h"
#include "asm/semaphore.h"
#include "asm/uaccess.h"
#include "asm/checksum.h"
#include "asm/errno.h"

EXPORT_SYMBOL(__down_failed);
EXPORT_SYMBOL(__down_failed_interruptible);
EXPORT_SYMBOL(__down_failed_trylock);
EXPORT_SYMBOL(__up_wakeup);

/*XXX: we need them because they would be exported by x86_64 */
#if (__GNUC__ == 4 && __GNUC_MINOR__ >= 3) || __GNUC__ > 4
EXPORT_SYMBOL(memcpy);
#else
EXPORT_SYMBOL(__memcpy);
#endif
EXPORT_SYMBOL(csum_partial);
