#include <algorithm>
#include "assert.h"
#include "Combiner.h"
#include "Config.h"
#include "Debugger.h"
#include "DisplayWindow.h"
#include "FrameBuffer.h"
#include "GLideN64.h"
#include "math.h"
#include "Platform.h"
#include "RSP.h"

Debugger g_debugger;

using namespace graphics;

Debugger::Debugger() : m_bDebugMode(false) {}
Debugger::~Debugger() {}
void Debugger::checkDebugState() {}
void Debugger::addTriangles(const graphics::Context::DrawTriangleParameters &) {}
void Debugger::addRects(const graphics::Context::DrawRectParameters &) {}
void Debugger::draw() {}
