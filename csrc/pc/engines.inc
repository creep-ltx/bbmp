// SPDX-License-Identifier: GPL-3.0-or-later
// The engines of bbmp-pc: name, as given on the command line. Included by
// bbmp-pc.cpp; tools/build-pc.sh defines WITH_<ENGINE> for those it built.
#ifdef WITH_UNITRK
ENGINE(unitrk)
#endif
#ifdef WITH_PLAYERPRO
ENGINE(playerpro)
#endif
#ifdef WITH_BUZZ
ENGINE(buzz)
#endif
#ifdef WITH_DSM
ENGINE(dsm)
#endif
#ifdef WITH_MXM
ENGINE(mxm)
#endif
