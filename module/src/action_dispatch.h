/* action_dispatch.h -- see action_dispatch.c: calling Doom's action functions safely under WebAssembly. */

#ifndef TRACEDOOM_ACTION_DISPATCH_H
#define TRACEDOOM_ACTION_DISPATCH_H

/* Calls one state's action. player and psp are NULL when it comes from a thing rather than a weapon. */
void tracedoom_call_action(void *action, void *mobj, void *player, void *psp);

#endif
