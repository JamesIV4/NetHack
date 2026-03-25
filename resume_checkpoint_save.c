#include "hack.h"

#include <string.h>
#include <unistd.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

#if defined(__EMSCRIPTEN__) && defined(SELF_RECOVER)
/* Browser bridge for converting checkpoint shards into a regular save
 * before startup continues through NetHack's normal restore flow.
 */
EMSCRIPTEN_KEEPALIVE
int
resume_checkpoint_save(const char *player_name)
{
    if (!player_name || !*player_name)
        return 0;

    (void) strncpy(svp.plname, player_name, sizeof svp.plname - 1);
    svp.plname[sizeof svp.plname - 1] = '\0';

    Sprintf(gl.lock, "%u%s", (unsigned) getuid(), svp.plname);
    regularize(gl.lock);

    return recover_savefile();
}
#endif
