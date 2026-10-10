// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-darkwave: DarkWave Studio's machines draw their own windows; without them
// only the names of the window classes are left, for the pointers that still hold them.
// Part of a port of DarkWave Studio 4.1.6 (GPL 3 or later, see csrc/darkwave/README.md).
#pragma once
class CFrameBuffer { public: CFrameBuffer(int) {} };
class CBitFont { public: template <class... A> CBitFont(A...) {} };
class CColorManager;
class CControl;
class CLiteContainer;
class CLiteControl;
class CLiteSlider;
class CLiteKnob;
class CLiteButton;
class CLiteLCDText;
class CToolBar;
class CToolBarButton;
class CSampleView;
class CBackBuffer;
class CToolTip;
class CImpulseView;
class CRoomView;
