// Scintilla source code edit control
/** @file EditView.cxx
 ** Defines the appearance of the main text area of the editor window.
 **/
// Copyright 1998-2014 by Neil Hodgson <neilh@scintilla.org>
// The License.txt file describes the conditions under which this software may be distributed.

#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <cmath>

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <forward_list>
#include <optional>
#include <algorithm>
#include <iterator>
#include <memory>
#include <chrono>

#include <atomic>
//#include <future>
#include <windows.h>
#ifndef _WIN32_WINNT_VISTA
#define _WIN32_WINNT_VISTA	0x0600
#endif

#include "ScintillaTypes.h"
#include "ScintillaMessages.h"
#include "ScintillaStructures.h"
#include "ILoader.h"
#include "ILexer.h"

#include "Debugging.h"
#include "Geometry.h"
#include "Platform.h"
#include "VectorISA.h"
#include "GraphicUtils.h"

#include "CharacterSet.h"
//#include "CharacterCategory.h"
#include "Position.h"
#include "UniqueString.h"
#include "SplitVector.h"
#include "Partitioning.h"
#include "RunStyles.h"
#include "ContractionState.h"
#include "CellBuffer.h"
#include "PerLine.h"
#include "KeyMap.h"
#include "Indicator.h"
#include "LineMarker.h"
#include "Style.h"
#include "ViewStyle.h"
#include "CharClassify.h"
#include "Decoration.h"
#include "CaseFolder.h"
#include "Document.h"
#include "UniConversion.h"
#include "Selection.h"
#include "PositionCache.h"
#include "EditModel.h"
#include "MarginView.h"
#include "EditView.h"
#include "ElapsedPeriod.h"

using namespace Scintilla;
using namespace Scintilla::Internal;
using namespace Lexilla;

PrintParameters::PrintParameters() noexcept {
	magnification = 100;
	colourMode = PrintOption::Normal;
	wrapState = Wrap::Word;
}

namespace Scintilla::Internal {

#if NP2_USE_AVX2
inline ColourRGBA MixAlpha(ColourRGBA colour, ColourRGBA other) noexcept {
	__m128i i16x4Fore = unpack_color_epi16_sse4_si32(other.AsInteger());
	const __m128i i16x4Back = unpack_color_epi16_sse4_si32(colour.AsInteger());
	const __m128i i16x4Alpha = _mm_shufflelo_epi16(i16x4Fore, _MM_SHUFFLE(3, 3, 3, 3));
	i16x4Fore = mm_alpha_blend_epi16(i16x4Fore, i16x4Back, i16x4Alpha);
	const uint32_t color = pack_color_epi16_sse2_si32(i16x4Fore);
	return ColourRGBA(color);
}

inline ColourRGBA AlphaBlend(ColourRGBA fore, ColourRGBA back, unsigned int alpha) noexcept {
	__m128i i16x4Fore = unpack_color_epi16_sse4_si32(fore.AsInteger());
	const __m128i i16x4Back = unpack_color_epi16_sse4_si32(back.AsInteger());
	const __m128i i16x4Alpha = mm_setlo_alpha_epi16(alpha);
	i16x4Fore = mm_alpha_blend_epi16(i16x4Fore, i16x4Back, i16x4Alpha);
	const uint32_t color = pack_color_epi16_sse2_si32(i16x4Fore);
	return ColourRGBA(color);
}

#elif NP2_USE_SSE2
inline ColourRGBA MixAlpha(ColourRGBA colour, ColourRGBA other) noexcept {
	__m128i i16x4Fore = unpack_color_epi16_sse2_si32(other.AsInteger());
	const __m128i i16x4Back = unpack_color_epi16_sse2_si32(colour.AsInteger());
	const __m128i i16x4Alpha = _mm_shufflelo_epi16(i16x4Fore, _MM_SHUFFLE(3, 3, 3, 3));
	i16x4Fore = mm_alpha_blend_epi16(i16x4Fore, i16x4Back, i16x4Alpha);
	const uint32_t color = pack_color_epi16_sse2_si32(i16x4Fore);
	return ColourRGBA(color);
}

inline ColourRGBA AlphaBlend(ColourRGBA fore, ColourRGBA back, unsigned int alpha) noexcept {
	__m128i i16x4Fore = unpack_color_epi16_sse2_si32(fore.AsInteger());
	const __m128i i16x4Back = unpack_color_epi16_sse2_si32(back.AsInteger());
	const __m128i i16x4Alpha = mm_setlo_alpha_epi16(alpha);
	i16x4Fore = mm_alpha_blend_epi16(i16x4Fore, i16x4Back, i16x4Alpha);
	const uint32_t color = pack_color_epi16_sse2_si32(i16x4Fore);
	return ColourRGBA(color);
}

#else
constexpr ColourRGBA MixAlpha(ColourRGBA colour, ColourRGBA other) noexcept {
	return ColourRGBA::MixAlpha(colour, other);
}

constexpr ColourRGBA AlphaBlend(ColourRGBA fore, ColourRGBA back, unsigned int alpha) noexcept {
	return ColourRGBA::AlphaBlend(fore, back, alpha);
}
#endif

bool ValidStyledText(const ViewStyle &vs, size_t styleOffset, const StyledText &st) noexcept {
	if (st.multipleStyles) {
		for (size_t iStyle = 0; iStyle < st.length; iStyle++) {
			if (!vs.ValidStyle(styleOffset + st.styles[iStyle]))
				return false;
		}
	} else {
		if (!vs.ValidStyle(styleOffset + st.style))
			return false;
	}
	return true;
}

int WidthStyledText(Surface *surface, const ViewStyle &vs, int styleOffset,
	const char *text, const unsigned char *styles, size_t len) {
	XYPOSITION width = 0;
	size_t start = 0;
	while (start < len) {
		const unsigned char style = styles[start];
		size_t endSegment = start;
		while ((endSegment + 1 < len) && (styles[endSegment + 1] == style)) {
			endSegment++;
		}
		const Font *fontText = vs.styles[style + styleOffset].font.get();
		const std::string_view sv(text + start, endSegment - start + 1);
		width += surface->WidthText(fontText, sv);
		start = endSegment + 1;
	}
	return static_cast<int>(std::lround(width));
}

int WidestLineWidth(Surface *surface, const ViewStyle &vs, int styleOffset, const StyledText &st) {
	int widthMax = 0;
	size_t start = 0;
	while (start < st.length) {
		const size_t lenLine = st.LineLength(start);
		int widthSubLine;
		if (st.multipleStyles) {
			widthSubLine = WidthStyledText(surface, vs, styleOffset, st.text + start, st.styles + start, lenLine);
		} else {
			const Font *fontText = vs.styles[styleOffset + st.style].font.get();
			const std::string_view text(st.text + start, lenLine);
			widthSubLine = static_cast<int>(std::lround(surface->WidthText(fontText, text)));
		}
		widthMax = std::max(widthMax, widthSubLine);
		start += lenLine + 1;
	}
	return widthMax;
}

void DrawTextNoClipPhase(Surface *surface, PRectangle rc, const Style &style, XYPOSITION ybase,
	std::string_view text, DrawPhase phase) {
	const Font *fontText = style.font.get();
	if (FlagSet(phase, DrawPhase::back)) {
		if (FlagSet(phase, DrawPhase::text)) {
			// Drawing both
			surface->DrawTextNoClip(rc, fontText, ybase, text,
				style.fore, style.back);
		} else {
			surface->FillRectangleAligned(rc, Fill(style.back));
		}
	} else if (FlagSet(phase, DrawPhase::text)) {
		surface->DrawTextTransparent(rc, fontText, ybase, text, style.fore);
	}
}

void DrawStyledText(Surface *surface, const ViewStyle &vs, int styleOffset, PRectangle rcText,
	const StyledText &st, size_t start, size_t length, DrawPhase phase) {

	if (st.multipleStyles) {
		XYPOSITION x = rcText.left;
		size_t i = 0;
		while (i < length) {
			size_t end = i;
			size_t style = st.styles[i + start];
			while (end < length - 1 && st.styles[start + end + 1] == style) {
				end++;
			}
			style += styleOffset;
			const Font *fontText = vs.styles[style].font.get();
			const std::string_view text(st.text + start + i, end - i + 1);
			const XYPOSITION width = surface->WidthText(fontText, text);
			PRectangle rcSegment = rcText;
			rcSegment.left = x;
			rcSegment.right = x + width + 1;
			DrawTextNoClipPhase(surface, rcSegment, vs.styles[style],
				rcText.top + vs.maxAscent, text, phase);
			x += width;
			i = end + 1;
		}
	} else {
		const size_t style = st.style + styleOffset;
		DrawTextNoClipPhase(surface, rcText, vs.styles[style],
			rcText.top + vs.maxAscent,
			std::string_view(st.text + start, length), phase);
	}
}

}

EditView::EditView() {
	tabWidthMinimumPixels = 2; // needed for calculating tab stops for fractional proportional fonts
	drawOverstrikeCaret = true;
	bufferedDraw = true;
	phasesDraw = PhasesDraw::Two;
	lineWidthMaxSeen = 0;
	additionalCaretsBlink = true;
	additionalCaretsVisible = true;
	imeCaretBlockOverride = false;
	llc.SetLevel(LineCache::Caret);
	tabArrowHeight = 4;
	customDrawTabArrow = nullptr;
	customDrawWrapMarker = nullptr;
}

EditView::~EditView() = default;

bool EditView::SetTwoPhaseDraw(bool twoPhaseDraw) noexcept {
	const PhasesDraw phasesDrawNew = twoPhaseDraw ? PhasesDraw::Two : PhasesDraw::One;
	const bool redraw = phasesDraw != phasesDrawNew;
	phasesDraw = phasesDrawNew;
	return redraw;
}

bool EditView::SetPhasesDraw(int phases) noexcept {
	const PhasesDraw phasesDrawNew = static_cast<PhasesDraw>(phases);
	const bool redraw = phasesDraw != phasesDrawNew;
	phasesDraw = phasesDrawNew;
	return redraw;
}

bool EditView::LinesOverlap() const noexcept {
	return phasesDraw == PhasesDraw::Multiple;
}

void EditView::ClearAllTabstops() noexcept {
	ldTabstops.reset();
}

XYPOSITION EditView::NextTabstopPos(Sci::Line line, XYPOSITION x, XYPOSITION tabWidth) const noexcept {
	const int next = GetNextTabstop(line, static_cast<int>(x + tabWidthMinimumPixels));
	if (next > 0)
		return static_cast<XYPOSITION>(next);
	return (static_cast<int>((x + tabWidthMinimumPixels) / tabWidth) + 1) * tabWidth;
}

bool EditView::ClearTabstops(Sci::Line line) const noexcept {
	return ldTabstops && ldTabstops->ClearTabstops(line);
}

bool EditView::AddTabstop(Sci::Line line, int x) {
	if (!ldTabstops) {
		ldTabstops = std::make_unique<LineTabstops>();
	}
	return ldTabstops && ldTabstops->AddTabstop(line, x);
}

int EditView::GetNextTabstop(Sci::Line line, int x) const noexcept {
	if (ldTabstops) {
		return ldTabstops->GetNextTabstop(line, x);
	} else {
		return 0;
	}
}

void EditView::LinesAddedOrRemoved(Sci::Line lineOfPos, Sci::Line linesAdded) const {
	if (ldTabstops) {
		if (linesAdded > 0) {
			for (Sci::Line line = lineOfPos; line < lineOfPos + linesAdded; line++) {
				ldTabstops->InsertLine(line);
			}
		} else {
			for (Sci::Line line = (lineOfPos + -linesAdded) - 1; line >= lineOfPos; line--) {
				ldTabstops->RemoveLine(line);
			}
		}
	}
}

void EditView::DropGraphics() noexcept {
	pixmapLine.reset();
	pixmapIndentGuide.reset();
	pixmapIndentGuideHighlight.reset();
}

static const char *ControlCharacterString(unsigned char ch) noexcept {
	static const char * const reps[] = {
		"NUL", "SOH", "STX", "ETX", "EOT", "ENQ", "ACK", "BEL",
		"BS", "HT", "LF", "VT", "FF", "CR", "SO", "SI",
		"DLE", "DC1", "DC2", "DC3", "DC4", "NAK", "SYN", "ETB",
		"CAN", "EM", "SUB", "ESC", "FS", "GS", "RS", "US"
	};
	if (ch < std::size(reps)) {
		return reps[ch];
	} else {
		return "BAD";
	}
}

static void DrawTabArrow(Surface *surface, PRectangle rcTab, int ymid,
	const ViewStyle &vsDraw, Stroke stroke) {

	const XYPOSITION halfWidth = stroke.width / 2.0;

	const XYPOSITION leftStroke = std::round(std::min(rcTab.left + 2, rcTab.right - 1)) + halfWidth;
	const XYPOSITION rightStroke = std::max(leftStroke, std::round(rcTab.right) - 1.0f - halfWidth);
	const XYPOSITION yMidAligned = ymid + halfWidth;
	const Point arrowPoint(rightStroke, yMidAligned);
	if (rightStroke > leftStroke) {
		// When not enough room, don't draw the arrow shaft
		surface->LineDraw(Point(leftStroke, yMidAligned), arrowPoint, stroke);
	}

	// Draw the arrow head if needed
	if (vsDraw.tabDrawMode == TabDrawMode::LongArrow) {
		XYPOSITION ydiff = std::floor(rcTab.Height() / 2.0f);
		XYPOSITION xhead = rightStroke - ydiff;
		if (xhead <= rcTab.left) {
			ydiff -= rcTab.left - xhead;
			xhead = rcTab.left;
		}
		const Point ptsHead[] = {
			Point(xhead, yMidAligned - ydiff),
			arrowPoint,
			Point(xhead, yMidAligned + ydiff)
		};
		surface->PolyLine(ptsHead, std::size(ptsHead), stroke);
	}
}

void EditView::RefreshPixMaps(Surface *surfaceWindow, const ViewStyle &vsDraw) {
	if (!pixmapIndentGuide) {
		// 1 extra pixel in height so can handle odd/even positions and so produce a continuous line
		pixmapIndentGuide = surfaceWindow->AllocatePixMap(1, vsDraw.lineHeight + 1);
		pixmapIndentGuideHighlight = surfaceWindow->AllocatePixMap(1, vsDraw.lineHeight + 1);
		const PRectangle rcIG = PRectangle::FromInts(0, 0, 1, vsDraw.lineHeight);
		pixmapIndentGuide->FillRectangle(rcIG, vsDraw.styles[StyleIndentGuide].back);
		pixmapIndentGuideHighlight->FillRectangle(rcIG, vsDraw.styles[StyleBraceLight].back);
		for (int stripe = 1; stripe < vsDraw.lineHeight + 1; stripe += 2) {
			const PRectangle rcPixel = PRectangle::FromInts(0, stripe, 1, stripe + 1);
			pixmapIndentGuide->FillRectangle(rcPixel, vsDraw.styles[StyleIndentGuide].fore);
			pixmapIndentGuideHighlight->FillRectangle(rcPixel, vsDraw.styles[StyleBraceLight].fore);
		}
		pixmapIndentGuide->FlushDrawing();
		pixmapIndentGuideHighlight->FlushDrawing();
	}
}

LineLayout *EditView::RetrieveLineLayout(Sci::Line lineNumber, const EditModel &model) {
	const Sci::Position posLineStart = model.pdoc->LineStart(lineNumber);
	const Sci::Position posLineEnd = model.pdoc->LineStart(lineNumber + 1);
	PLATFORM_ASSERT(posLineEnd >= posLineStart);
	const Sci::Position caretPosition = model.sel.MainCaret();
	const Sci::Line lineCaret = model.pdoc->SciLineFromPosition(caretPosition);
	const Sci::Line topLine = model.pcs->DocFromDisplay(model.TopLineOfMain());
	LineLayout *ll = llc.Retrieve(lineNumber, lineCaret,
		static_cast<int>(posLineEnd - posLineStart), model.pdoc->GetStyleClock(),
		model.LinesOnScreen() + 1, model.pdoc->LinesTotal(), topLine);
	if (lineNumber == lineCaret) {
		ll->caretPosition = static_cast<int>(caretPosition - posLineStart);
	} else {
		ll->caretPosition = 0;
	}
	return ll;
}

namespace {

constexpr XYPOSITION epsilon = 0.0001f;	// A small nudge to avoid floating point precision issues

enum class WrapBreak {
	None = 0,
	Before = 1,
	After = 2,
	Both = 3,
	Undefined,
};

constexpr uint8_t ASCIIWrapBreakTable[128] = {
//++Autogenerated -- start of section automatically generated
// Created with Python 3.11.0rc2, Unicode 14.0.0
2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
2, 2, 0, 1, 1, 2, 2, 0, 1, 2, 2, 1, 2, 2, 2, 2,
0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 1, 2, 2, 2,
1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 0,
0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 2, 1, 2,
//--Autogenerated -- end of section automatically generated
};

constexpr WrapBreak GetWrapBreak(unsigned char ch) noexcept {
	return (ch & 0x80)? WrapBreak::None : static_cast<WrapBreak>(ASCIIWrapBreakTable[ch]);
}

constexpr WrapBreak GetWrapBreakEx(unsigned int ch, bool isUtf8) noexcept {
	if (ch < 0x80) {
		return static_cast<WrapBreak>(ASCIIWrapBreakTable[ch]);
	}
	if (isUtf8) {
		// fullwidth forms
		if (ch > 0xFF00 && ch < 0xFF5F) {
			return static_cast<WrapBreak>(ASCIIWrapBreakTable[ch - 0xFEE0]);
		}
	}

	return WrapBreak::Undefined;
}

struct LayoutWorker {
	LineLayout * const ll;
	const ViewStyle &vstyle;
	Surface * const sharedSurface;
	PositionCache &posCache;
	const EditModel &model;

	std::vector<TextSegment> segmentList;
	uint32_t segmentCount = 0;
	int maxPosInLine = 0;
	std::atomic<uint32_t> nextIndex = 0;
	std::atomic<uint32_t> finishedCount = 0;

#define USE_STD_ASYNC_FUTURE	0
#if USE_STD_ASYNC_FUTURE
#define USE_WIN32_PTP_WORK		0
#define USE_WIN32_WORK_ITEM		0
#elif _WIN32_WINNT >= _WIN32_WINNT_VISTA
#define USE_WIN32_PTP_WORK		1
#define USE_WIN32_WORK_ITEM		0
#else
#define USE_WIN32_PTP_WORK		0
#define USE_WIN32_WORK_ITEM		1
#endif

#if USE_WIN32_WORK_ITEM
	HANDLE finishedEvent = nullptr;
	std::atomic<uint32_t> runningThread = 0;
#endif

	static constexpr int blockSize = 4096;

	template<typename T>
	static inline void UpdateMaximum(std::atomic<T> &maximum, const T &value) noexcept {
		// https://stackoverflow.com/questions/16190078/how-to-atomically-update-a-maximum-value
		T prev = maximum;
		while(prev < value && !maximum.compare_exchange_weak(prev, value)) {}
	}

	void Layout(const TextSegment &ts, Surface *surface) {
		const Style &style = vstyle.styles[ll->styles[ts.start]];
		if (style.visible) {
			if (ts.representation) {
				if (ll->chars[ts.start] == '\t') {
					// Tab is a special case of representation, taking a variable amount of space
					// which will be filled in later.
				} else {
					XYPOSITION representationWidth = vstyle.controlCharWidth;
					if (representationWidth <= 0.0) {
						const Style &styleCtrl = vstyle.styles[StyleControlChar];
						// only supports character representation with ASCII text
						if (styleCtrl.monospaceASCII) {
							representationWidth = style.aveCharWidth * ts.representation->length;
						} else {
							XYPOSITION positionsRepr[Representation::maxLength + 1];
							const std::string_view stringRep = ts.representation->GetStringRep();
							posCache.MeasureWidths(surface, styleCtrl, StyleControlChar, stringRep, positionsRepr);
							representationWidth = positionsRepr[ts.representation->length - 1];
						}
						if (FlagSet(ts.representation->appearance, RepresentationAppearance::Blob)) {
							representationWidth += vstyle.ctrlCharPadding;
						}
					}
					for (int ii = 0; ii < ts.length; ii++) {
						ll->positions[ts.start + 1 + ii] = representationWidth;
					}
				}
			} else {
				if ((ts.length == 1) && (' ' == ll->chars[ts.start])) {
					// Over half the segments are single characters and of these about half are space characters.
					ll->positions[ts.start + 1] = style.spaceWidth;
				} else {
					posCache.MeasureWidths(surface, style, ll->styles[ts.start],
						std::string_view(&ll->chars[ts.start], ts.length), &ll->positions[ts.start + 1]);
				}
			}
		}
	}

	uint32_t Start(Sci::Position posLineStart, uint32_t posInLine) {
		const int startPos = ll->lastSegmentEnd;
		const int endPos = ll->numCharsInLine;
		if (endPos - startPos > blockSize*2 && !model.BidirectionalEnabled()) {
			posInLine = std::max<uint32_t>(posInLine, ll->caretPosition) + blockSize;
			if (posInLine > static_cast<uint32_t>(endPos)) {
				posInLine = endPos;
			}
		} else {
			posInLine = endPos;
		}

		BreakFinder bfLayout(ll, nullptr, Range(startPos, endPos), posLineStart, 0, BreakFinder::BreakFor::Layout, model, nullptr, posInLine);
		do {
			segmentList.push_back(bfLayout.Next());
		} while (bfLayout.More());

		maxPosInLine = static_cast<int>(posInLine);
		const uint32_t length = bfLayout.CurrentPos() - startPos;
		if (length >= model.minParallelLayoutLength && model.hardwareConcurrency > 1) {
			segmentCount = static_cast<uint32_t>(segmentList.size());
			const uint32_t threadCount = std::min(length/blockSize, model.hardwareConcurrency);
#if USE_STD_ASYNC_FUTURE
			std::vector<std::future<void>> features;
			for (uint32_t i = 0; i < threadCount; i++) {
				features.push_back(std::async(std::launch::async, [this] {
					DoWork();
				}));
			}
			for (std::future<void> &f : features) {
				f.wait();
			}

#elif USE_WIN32_PTP_WORK
			PTP_WORK work = CreateThreadpoolWork(WorkCallback, this, nullptr);
			for (uint32_t i = 0; i < threadCount; i++) {
				SubmitThreadpoolWork(work);
			}
			WaitForThreadpoolWorkCallbacks(work, FALSE);
			CloseThreadpoolWork(work);

#else
			runningThread.store(threadCount, std::memory_order_relaxed);
			finishedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			for (uint32_t i = 0; i < threadCount; i++) {
				QueueUserWorkItem(ThreadProc, this, WT_EXECUTEDEFAULT);
			}
			while (true) {
				const DWORD result = WaitForSingleObject(finishedEvent, 0);
				if (result == WAIT_OBJECT_0) {
					if (runningThread.load(std::memory_order_relaxed) == 0) {
						break;
					}
				} else if (result == WAIT_TIMEOUT) {
				}
				SwitchToThread();
				//Sleep(0);
				//YieldProcessor();
			}
			CloseHandle(finishedEvent);
#endif // USE_WIN32_WORK_ITEM
			return threadCount;
		}

		void * const idleTaskTimer = model.idleTaskTimer;
		Surface * const surface = sharedSurface;
		int processed = 0;
		auto it = segmentList.begin();
		while (true) {
			const TextSegment &ts = *it++;
			Layout(ts, surface);
			if (it == segmentList.end()) {
				break;
			}
			processed += ts.length;
			if (processed >= blockSize) {
				processed = 0;
				if (ts.end() > maxPosInLine && WaitForSingleObject(idleTaskTimer, 0) == WAIT_OBJECT_0) {
					break;
				}
			}
		}

		finishedCount.store(static_cast<uint32_t>(it - segmentList.begin()), std::memory_order_relaxed);
		return 1;
	}

	std::unique_ptr<Surface> CreateMeasurementSurface() const {
		// if (!surface->SupportsFeature(Supports::ThreadSafeMeasureWidths))
		if (vstyle.technology == Technology::Default) {
			std::unique_ptr<Surface> surf = Surface::Allocate(Technology::Default);
			surf->Init(nullptr);
			surf->SetMode(model.CurrentSurfaceMode());
			return surf;
		}
		return {};
	}

	void DoWork() {
		uint32_t finished = 0;
		void * const idleTaskTimer = model.idleTaskTimer;
		const std::unique_ptr<Surface> surf{CreateMeasurementSurface()};
		Surface * const surface = surf ? surf.get() : sharedSurface;

		int processed = 0;
		while (true) {
			const uint32_t index = nextIndex.fetch_add(1, std::memory_order_relaxed);
			if (index >= segmentCount) {
				break;
			}

			const TextSegment &ts = segmentList[index];
			Layout(ts, surface);
			finished = index + 1;
			processed += ts.length;
			if (processed >= blockSize) {
				processed = 0;
				if (ts.end() > maxPosInLine && WaitForSingleObject(idleTaskTimer, 0) == WAIT_OBJECT_0) {
					break;
				}
			}
		}

		UpdateMaximum(finishedCount, finished);
#if USE_WIN32_WORK_ITEM
		SetEvent(finishedEvent);
		runningThread.fetch_sub(1, std::memory_order_relaxed);
#endif
	}

#if USE_WIN32_PTP_WORK
	static VOID CALLBACK WorkCallback([[maybe_unused]] PTP_CALLBACK_INSTANCE instance, PVOID context, [[maybe_unused]] PTP_WORK work) {
		LayoutWorker *worker = static_cast<LayoutWorker *>(context);
		worker->DoWork();
	}
#elif USE_WIN32_WORK_ITEM
	static DWORD WINAPI ThreadProc(LPVOID lpParameter) {
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
		LayoutWorker *worker = static_cast<LayoutWorker *>(lpParameter);
		worker->DoWork();
		return 0;
	}
#endif
};

}

/**
* Fill in the LineLayout data for the given line.
* Copy the given @a line and its styles from the document into local arrays.
* Also determine the x position at which each character starts.
*/
uint64_t EditView::LayoutLine(const EditModel &model, Surface *surface, const ViewStyle &vstyle, LineLayout *ll, int width, LayoutLineOption option, int posInLine) {
	uint64_t wrappedBytes = 0; // only care about time spend on MeasureWidths()
	const Sci::Line line = ll->LineNumber();
	PLATFORM_ASSERT(line < model.pdoc->LinesTotal());
	PLATFORM_ASSERT(ll->chars);
	const Sci::Position posLineStart = model.pdoc->LineStart(line);
	// If the line is very long, limit the treatment to a length that should fit in the viewport
	const Sci::Position posLineEnd = std::min(model.pdoc->LineStart(line + 1), posLineStart + ll->maxLineLength);
	// Hard to cope when too narrow, so just assume there is space
	width = std::max(width, 20);

	auto validity = ll->validity;
	if (validity == LineLayout::ValidLevel::checkTextAndStyle) {
		const Sci::Position lineLength = (vstyle.viewEOL ? posLineEnd : model.pdoc->LineEnd(line)) - posLineStart;
		validity = LineLayout::ValidLevel::invalid;
		if (lineLength == ll->numCharsInLine) {
			//const ElapsedPeriod period;
			// See if chars, styles, indicators, are all the same
			int allSame = 0;
			// Check base line layout
			const uint8_t *styles = ll->styles.get();

			if (lineLength != 0) {
				const char *docStyles = model.pdoc->StyleRangePointer(posLineStart, lineLength);
				if (docStyles) { // HasStyles
					allSame = memcmp(docStyles, styles, lineLength);
				}

				const char *docChars = model.pdoc->RangePointer(posLineStart, lineLength);
				const char *chars = ll->chars.get();
				// NOLINTNEXTLINE(bugprone-suspicious-string-compare)
				allSame |= memcmp(docChars, chars, lineLength);
			}

			const int styleByteLast = (posLineEnd == posLineStart) ? 0 : model.pdoc->StyleIndexAt(posLineEnd - 1);
			allSame |= styles[lineLength] ^ styleByteLast; // For eolFilled
			//const double duration = period.Duration()*1e3;
			//printf("check line=%zd (%zd) allSame=%d, duration=%f\n", line + 1, lineLength, allSame, duration);
			if (allSame == 0) {
				validity = (ll->widthLine != width)? LineLayout::ValidLevel::positions : LineLayout::ValidLevel::lines;
			}
		}
	}
	if (validity == LineLayout::ValidLevel::invalid) {
		// Fill base line layout
		const int lineLength = static_cast<int>(posLineEnd - posLineStart);
		model.pdoc->GetCharRange(ll->chars.get(), posLineStart, lineLength);
		model.pdoc->GetStyleRange(ll->styles.get(), posLineStart, lineLength);
		const int numCharsBeforeEOL = static_cast<int>(model.pdoc->LineEnd(line) - posLineStart);
		const int numCharsInLine = vstyle.viewEOL ? lineLength : numCharsBeforeEOL;
		const unsigned char styleByteLast = (lineLength == 0) ? 0 : ll->styles[lineLength - 1];
		// Extra element at the end of the line to hold end x position and act as
		ll->chars[numCharsInLine] = 0;   // Also triggers processing in the loops as this is a control character
		ll->styles[numCharsInLine] = styleByteLast;	// For eolFilled

		// Layout the line, determining the position of each character,
		// with an extra element at the end for the end of the line.
		ll->ClearPositions();
		ll->lastSegmentEnd = 0;
		ll->numCharsInLine = numCharsInLine;
		ll->numCharsBeforeEOL = numCharsBeforeEOL;

		ll->xHighlightGuide = 0;
		ll->edgeColumn = -1;
		ll->widthLine = LineLayout::wrapWidthInfinite;
		ll->lines = 1;
		if (numCharsInLine == 0) {
			// empty line with viewEOL disabled
			ll->widthLine = width;
			ll->wrapIndent = 0;
			validity = LineLayout::ValidLevel::lines;
		} else if (vstyle.edgeState == EdgeVisualStyle::Background) {
			Sci::Position edgePosition = model.pdoc->FindColumn(line, vstyle.theEdge.column);
			if (edgePosition >= posLineStart) {
				edgePosition -= posLineStart;
			}
			ll->edgeColumn = static_cast<int>(edgePosition);
		}
	}

	const bool partialLine = validity == LineLayout::ValidLevel::lines
		&& ll->PartialPosition() && width == ll->widthLine;
	if (validity == LineLayout::ValidLevel::invalid
		|| (option != LayoutLineOption::KeepPosition && ll->PartialPosition())) {
		//if (ll->maxLineLength > LayoutWorker::blockSize) {
		//	printf("start layout line=%zd, posInLine=%d\n", line + 1, posInLine);
		//}
		//const ElapsedPeriod period;
		//posInLine = ll->numCharsInLine; // whole line
		LayoutWorker worker{ ll, vstyle, surface, posCache, model, {}};
		const uint32_t threadCount = worker.Start(posLineStart, posInLine);

		// Accumulate absolute positions from relative positions within segments and expand tabs
		const uint32_t finishedCount = worker.finishedCount.load(std::memory_order_relaxed);
		uint32_t iByte = ll->lastSegmentEnd;
		XYPOSITION xPosition = ll->positions[iByte++];
		for (auto it = worker.segmentList.begin(); it != worker.segmentList.begin() + finishedCount; ++it) {
			const TextSegment &ts = *it;
			if (ts.representation && (ll->chars[ts.start] == '\t') && vstyle.styles[ll->styles[ts.start]].visible) {
				// Simple visible tab, go to next tab stop
				const XYPOSITION startTab = ll->positions[ts.start];
				const XYPOSITION nextTab = NextTabstopPos(line, startTab, vstyle.tabWidth);
				xPosition += nextTab - startTab;
			}

			const XYPOSITION xBeginSegment = xPosition;
			for (int i = 0; i < ts.length; i++) {
				xPosition = ll->positions[iByte] + xBeginSegment;
				ll->positions[iByte++] = xPosition;
			}
		}

		const TextSegment &ts = worker.segmentList[finishedCount - 1];
		const int endPos = ts.end();
		const uint32_t bytes = endPos - ll->lastSegmentEnd;
		wrappedBytes = bytes | (static_cast<uint64_t>(bytes / threadCount) << 32);
#if 0
		if (bytes > LayoutWorker::blockSize) {
			const double duration = period.Duration()*1e3;
			printf("layout line=%zd segment=(%u / %zu), posInLine=(%d / %d) (%u / %u, %u), duration=%f, %f\n", line + 1,
				finishedCount, worker.segmentList.size(), worker.maxPosInLine, ll->maxLineLength,
				bytes, threadCount, bytes / threadCount, duration, model.durationWrapOneThread.Duration()*1e3);
		}
#endif
		ll->lastSegmentEnd = endPos;
		if (endPos == ll->numCharsInLine) {
			// Small hack to make lines that end with italics not cut off the edge of the last character
			// Not quite the same as before which would effectively ignore trailing invisible segments
			if (!ts.representation && (ll->chars[endPos - 1] != ' ') && vstyle.styles[ll->styles[ts.start]].italic) {
				ll->positions[endPos] += vstyle.lastSegItalicsOffset;
			}
		}
		validity = LineLayout::ValidLevel::positions;
	}
	if ((validity == LineLayout::ValidLevel::positions) || (ll->widthLine != width)) {
		const int linesWrapped = ll->lines;
		ll->widthLine = width;
		if (width == LineLayout::wrapWidthInfinite) {
			ll->lines = 1;
		} else if (width > ll->positions[ll->lastSegmentEnd]) {
			// Simple common case where line does not need wrapping.
			ll->lines = 1;
		} else {
			//const ElapsedPeriod period;
			XYPOSITION wrapIndent = ll->wrapIndent;
			const XYPOSITION aveCharWidth = vstyle.aveCharWidth;
			if (FlagSet(vstyle.wrap.visualFlags, WrapVisualFlag::End)) {
				width -= static_cast<int>(aveCharWidth); // take into account the space for end wrap mark
			}
			XYPOSITION wrapAddIndent = 0; // This will be added to initial indent of line
			switch (vstyle.wrap.indentMode) {
			case WrapIndentMode::Fixed:
				wrapAddIndent = vstyle.wrap.visualStartIndent * aveCharWidth;
				break;
			case WrapIndentMode::Indent:
				wrapAddIndent = model.pdoc->IndentSize() * aveCharWidth;
				break;
			case WrapIndentMode::DeepIndent:
				wrapAddIndent = model.pdoc->IndentSize() * 2 * aveCharWidth;
				break;
			default:	// No additional indent for WrapIndentMode::Fixed
				break;
			}
			ll->wrapIndent = wrapAddIndent;
			if (vstyle.wrap.indentMode != WrapIndentMode::Fixed) {
				for (int i = 0; i < ll->lastSegmentEnd; i++) {
					if (!IsSpaceOrTab(ll->chars[i])) {
						ll->wrapIndent += ll->positions[i]; // Add line indent
						break;
					}
				}
			}
			// Check for text width minimum
			if (ll->wrapIndent > width - static_cast<int>(aveCharWidth) * 15) {
				ll->wrapIndent = wrapAddIndent;
			}
			// Check for wrapIndent minimum
			if ((FlagSet(vstyle.wrap.visualFlags, WrapVisualFlag::Start)) && (ll->wrapIndent < vstyle.aveCharWidth)) {
				ll->wrapIndent = aveCharWidth; // Indent to show start visual
			}

			// Calculate line start positions based upon width.
			Sci::Position lastLineStart = 0;
			XYACCUMULATOR startOffset = width;
			Sci::Position p = 0;
			if (partialLine && ll->lines > 2 && wrapIndent == ll->wrapIndent) {
				lastLineStart = ll->LineStart(ll->lines - 2);
				ll->lines -= 2;
				p = lastLineStart + 1;
				startOffset += ll->positions[lastLineStart] - wrapIndent;
			} else {
				wrapIndent = ll->wrapIndent;
				ll->lines = 0;
			}

			const bool isUtf8 = CpUtf8 == model.pdoc->dbcsCodePage;
			const Wrap wrapState = vstyle.wrap.state;
			const Sci::Position numCharsInLine = ll->lastSegmentEnd;
			while (p < numCharsInLine) {
				while (p < numCharsInLine && ll->positions[p + 1] < startOffset) {
					p++;
				}
				if (p < numCharsInLine) {
					// backtrack to find lastGoodBreak
					Sci::Position lastGoodBreak = p;
					if (p > 0) {
						lastGoodBreak = model.pdoc->MovePositionOutsideChar(p + posLineStart, -1) - posLineStart;
					}
					if (wrapState != Wrap::Char) {
						Sci::Position pos = lastGoodBreak;
						CharacterClass ccPrev = CharacterClass::space;
						WrapBreak wbPrev = WrapBreak::None;
						if (wrapState == Wrap::Auto) {
							const int character = model.pdoc->CharacterAfter(pos + posLineStart).character;
							ccPrev = model.pdoc->WordCharacterClass(character);
							wbPrev = GetWrapBreakEx(character, isUtf8);
						} else if (wrapState == Wrap::Word) {
							wbPrev = GetWrapBreak(ll->chars[pos]);
						}
						while (pos > lastLineStart) {
							// style boundary and space
							if (wrapState != Wrap::WhiteSpace && (ll->styles[pos - 1] != ll->styles[pos])) {
								break;
							}
							if (IsBreakSpace(ll->chars[pos - 1]) && !IsBreakSpace(ll->chars[pos])) {
								break;
							}

							const Sci::Position posBefore = model.pdoc->MovePositionOutsideChar(pos + posLineStart - 1, -1) - posLineStart;
							if (wrapState == Wrap::Auto) {
								// word boundary
								// TODO: Unicode Line Breaking Algorithm https://www.unicode.org/reports/tr14/
								const WrapBreak wbPos = wbPrev;
								const CharacterClass ccPos = ccPrev;
								const int chPrevious = model.pdoc->CharacterAfter(posBefore + posLineStart).character;
								ccPrev = model.pdoc->WordCharacterClass(chPrevious);
								wbPrev = GetWrapBreakEx(chPrevious, isUtf8);
								if (wbPrev != WrapBreak::Before && wbPos != WrapBreak::After) {
									if ((ccPrev == CharacterClass::cjkWord || ccPos == CharacterClass::cjkWord) ||
										//(wbPrev == WrapBreak::Both || wbPos == WrapBreak::Both) ||
										(wbPrev != wbPos && (wbPrev == WrapBreak::After || wbPos == WrapBreak::Before)) ||
										(ccPrev != ccPos && (wbPrev == WrapBreak::Undefined || wbPos == WrapBreak::Undefined))
									) {
										break;
									}
								}
							} else if (wrapState == Wrap::Word) {
								const WrapBreak wbPos = wbPrev;
								wbPrev = GetWrapBreak(ll->chars[posBefore]);
								if (wbPrev != WrapBreak::Before && wbPos != WrapBreak::After) {
									if (//(wbPrev == WrapBreak::Both || wbPos == WrapBreak::Both) ||
										(wbPrev != wbPos && (wbPrev == WrapBreak::After || wbPos == WrapBreak::Before))
									) {
										break;
									}
								}
							}
							pos = posBefore;
						}
						if (pos > lastLineStart) {
							lastGoodBreak = pos;
						}
					}
					if (lastGoodBreak == lastLineStart) {
						// Try moving to start of last character
						if (p > 0) {
							lastGoodBreak = model.pdoc->MovePositionOutsideChar(p + posLineStart, -1) - posLineStart;
						}
						if (lastGoodBreak == lastLineStart) {
							// Ensure at least one character on line.
							lastGoodBreak = model.pdoc->MovePositionOutsideChar(lastGoodBreak + posLineStart + 1, 1) - posLineStart;
						}
					}
					lastLineStart = lastGoodBreak;
					ll->AddLineStart(lastLineStart);
					startOffset = ll->positions[lastLineStart];
					// take into account the space for start wrap mark and indent
					startOffset += width - wrapIndent;
					p = lastLineStart + 1;
				}
			}
			ll->lines++;
			//const double duration = period.Duration()*1e3;
			//printf("wrap line=%zd duration=%f\n", line + 1, duration);
		}

		validity = LineLayout::ValidLevel::lines;
		if (partialLine && option == LayoutLineOption::AutoUpdate && linesWrapped != ll->lines) {
			(const_cast<EditModel &>(model)).OnLineWrapped(line, ll->lines);
		}
	}
	ll->validity = validity;
	return wrappedBytes;
}

// Fill the LineLayout bidirectional data fields according to each char style

void EditView::UpdateBidiData(const EditModel &model, const ViewStyle &vstyle, LineLayout *ll) {
	if (model.BidirectionalEnabled()) {
		ll->EnsureBidiData();
		for (int stylesInLine = 0; stylesInLine < ll->numCharsInLine; stylesInLine++) {
			ll->bidiData->stylesFonts[stylesInLine] = vstyle.styles[ll->styles[stylesInLine]].font;
		}
		ll->bidiData->stylesFonts[ll->numCharsInLine].reset();

		for (int charsInLine = 0; charsInLine < ll->numCharsInLine; charsInLine++) {
			const int charWidth = UTF8DrawBytes(&ll->chars[charsInLine], ll->numCharsInLine - charsInLine);
			const Representation *repr = model.reprs.RepresentationFromCharacter(std::string_view(&ll->chars[charsInLine], charWidth));

			ll->bidiData->widthReprs[charsInLine] = 0.0f;
			if (repr && ll->chars[charsInLine] != '\t') {
				ll->bidiData->widthReprs[charsInLine] = ll->positions[charsInLine + charWidth] - ll->positions[charsInLine];
			}
			if (charWidth > 1) {
				for (int c = 1; c < charWidth; c++) {
					charsInLine++;
					ll->bidiData->widthReprs[charsInLine] = 0.0f;
				}
			}
		}
		ll->bidiData->widthReprs[ll->numCharsInLine] = 0.0f;
	} else {
		ll->bidiData.reset();
	}
}

Point EditView::LocationFromPosition(Surface *surface, const EditModel &model, SelectionPosition pos, Sci::Line topLine,
	const ViewStyle &vs, PointEnd pe, PRectangle rcClient) {
	Point pt;
	if (pos.Position() == Sci::invalidPosition)
		return pt;
	Sci::Line lineDoc = model.pdoc->SciLineFromPosition(pos.Position());
	Sci::Position posLineStart = model.pdoc->LineStart(lineDoc);
	if (FlagSet(pe, PointEnd::lineEnd) && (lineDoc > 0) && (pos.Position() == posLineStart)) {
		// Want point at end of first line
		lineDoc--;
		posLineStart = model.pdoc->LineStart(lineDoc);
	}
	if (surface) {
		LineLayout * const ll = RetrieveLineLayout(lineDoc, model);
		const int posInLine = static_cast<int>(pos.Position() - posLineStart);
		LayoutLine(model, surface, vs, ll, model.wrapWidth, LayoutLineOption::AutoUpdate, posInLine);
		pt = ll->PointFromPosition(posInLine, vs.lineHeight, pe);
		pt.x += vs.textStart - model.xOffset;

		if (model.BidirectionalEnabled()) {
			// Fill the line bidi data
			UpdateBidiData(model, vs, ll);

			// Find subLine
			const int subLine = ll->SubLineFromPosition(posInLine, pe);
			const int lineStart = ll->LineStart(subLine);
			const int caretPosition = posInLine - lineStart;

			// Get the point from current position
			const ScreenLine screenLine(ll, subLine, vs, rcClient.right, tabWidthMinimumPixels);
			const std::unique_ptr<IScreenLineLayout> slLayout = surface->Layout(&screenLine);
			pt.x = slLayout->XFromPosition(caretPosition);

			pt.x += vs.textStart - model.xOffset;

			pt.y = 0;
			if (posInLine >= lineStart) {
				pt.y = static_cast<XYPOSITION>(subLine*vs.lineHeight);
			}
		}

		const Sci::Line lineVisible = model.pcs->DisplayFromDoc(lineDoc);
		pt.y += (lineVisible - topLine) * vs.lineHeight;
		pt.x += pos.VirtualSpace() * vs.styles[ll->EndLineStyle()].spaceWidth;
	}
	return pt;
}

Range EditView::RangeDisplayLine(Surface *surface, const EditModel &model, Sci::Line lineVisible, const ViewStyle &vs) {
	Range rangeSubLine = Range(0, 0);
	if (lineVisible < 0) {
		return rangeSubLine;
	}
	const Sci::Line lineDoc = model.pcs->DocFromDisplay(lineVisible);
	const Sci::Position positionLineStart = model.pdoc->LineStart(lineDoc);
	if (surface) {
		LineLayout * const ll = RetrieveLineLayout(lineDoc, model);
		LayoutLine(model, surface, vs, ll, model.wrapWidth, LayoutLineOption::AutoUpdate);
		const Sci::Line lineStartSet = model.pcs->DisplayFromDoc(lineDoc);
		const int subLine = static_cast<int>(lineVisible - lineStartSet);
		if (subLine < ll->lines) {
			rangeSubLine = ll->SubLineRange(subLine, LineLayout::Scope::visibleOnly);
			if (subLine == ll->lines - 1) {
				rangeSubLine.end = model.pdoc->LineStart(lineDoc + 1) -
					positionLineStart;
			}
		}
	}
	rangeSubLine.start += positionLineStart;
	rangeSubLine.end += positionLineStart;
	return rangeSubLine;
}

SelectionPosition EditView::SPositionFromLocation(Surface *surface, const EditModel &model, PointDocument pt, bool canReturnInvalid,
	bool charPosition, bool virtualSpace, const ViewStyle &vs, PRectangle rcClient) {
	pt.x = pt.x - vs.textStart;
	Sci::Line visibleLine = static_cast<int>(std::floor(pt.y / vs.lineHeight));
	if (!canReturnInvalid && (visibleLine < 0))
		visibleLine = 0;
	const Sci::Line lineDoc = model.pcs->DocFromDisplay(visibleLine);
	if (canReturnInvalid && (lineDoc < 0))
		return SelectionPosition(Sci::invalidPosition);
	if (lineDoc >= model.pdoc->LinesTotal())
		return SelectionPosition(canReturnInvalid ? Sci::invalidPosition :
			model.pdoc->LengthNoExcept());
	const Sci::Position posLineStart = model.pdoc->LineStart(lineDoc);
	if (surface) {
		LineLayout * const ll = RetrieveLineLayout(lineDoc, model);
		LayoutLine(model, surface, vs, ll, model.wrapWidth, LayoutLineOption::AutoUpdate);
		const Sci::Line lineStartSet = model.pcs->DisplayFromDoc(lineDoc);
		const int subLine = static_cast<int>(visibleLine - lineStartSet);
		if (subLine < ll->lines) {
			const Range rangeSubLine = ll->SubLineRange(subLine, LineLayout::Scope::visibleOnly);
			const XYPOSITION subLineStart = ll->positions[rangeSubLine.start];
			if (subLine > 0)	// Wrapped
				pt.x -= ll->wrapIndent;
			Sci::Position positionInLine = 0;
			if (model.BidirectionalEnabled()) {
				// Fill the line bidi data
				UpdateBidiData(model, vs, ll);

				const ScreenLine screenLine(ll, subLine, vs, rcClient.right, tabWidthMinimumPixels);
				const std::unique_ptr<IScreenLineLayout> slLayout = surface->Layout(&screenLine);
				positionInLine = slLayout->PositionFromX(static_cast<XYPOSITION>(pt.x), charPosition) +
					rangeSubLine.start;
			} else {
				positionInLine = ll->FindPositionFromX(static_cast<XYPOSITION>(pt.x + subLineStart),
					rangeSubLine, charPosition);
			}
			if (positionInLine < rangeSubLine.end) {
				return SelectionPosition(model.pdoc->MovePositionOutsideChar(positionInLine + posLineStart, 1));
			}
			if (virtualSpace) {
				const XYPOSITION spaceWidth = vs.styles[ll->EndLineStyle()].spaceWidth;
				const int spaceOffset = static_cast<int>(
					(pt.x + subLineStart - ll->positions[rangeSubLine.end] + spaceWidth / 2) / spaceWidth);
				return SelectionPosition(rangeSubLine.end + posLineStart, spaceOffset);
			} else if (canReturnInvalid) {
				if (pt.x < (ll->positions[rangeSubLine.end] - subLineStart)) {
					return SelectionPosition(model.pdoc->MovePositionOutsideChar(rangeSubLine.end + posLineStart, 1));
				}
			} else {
				return SelectionPosition(rangeSubLine.end + posLineStart);
			}
		}
		if (!canReturnInvalid)
			return SelectionPosition(ll->numCharsInLine + posLineStart);
	}
	return SelectionPosition(canReturnInvalid ? Sci::invalidPosition : posLineStart);
}

/**
* Find the document position corresponding to an x coordinate on a particular document line.
* Ensure is between whole characters when document is in multi-byte or UTF-8 mode.
* This method is used for rectangular selections and does not work on wrapped lines.
*/
SelectionPosition EditView::SPositionFromLineX(Surface *surface, const EditModel &model, Sci::Line lineDoc, int x, const ViewStyle &vs) {
	if (surface) {
		LineLayout * const ll = RetrieveLineLayout(lineDoc, model);
		LayoutLine(model, surface, vs, ll, model.wrapWidth, LayoutLineOption::AutoUpdate);
		const Sci::Position posLineStart = model.pdoc->LineStart(lineDoc);
		const Range rangeSubLine = ll->SubLineRange(0, LineLayout::Scope::visibleOnly);
		const XYPOSITION subLineStart = ll->positions[rangeSubLine.start];
		const Sci::Position positionInLine = ll->FindPositionFromX(x + subLineStart, rangeSubLine, false);
		if (positionInLine < rangeSubLine.end) {
			return SelectionPosition(model.pdoc->MovePositionOutsideChar(positionInLine + posLineStart, 1));
		}
		const XYPOSITION spaceWidth = vs.styles[ll->EndLineStyle()].spaceWidth;
		const int spaceOffset = static_cast<int>(
			(x + subLineStart - ll->positions[rangeSubLine.end] + spaceWidth / 2) / spaceWidth);
		return SelectionPosition(rangeSubLine.end + posLineStart, spaceOffset);
	}
	return SelectionPosition(0);
}

Sci::Line EditView::DisplayFromPosition(Surface *surface, const EditModel &model, Sci::Position pos, const ViewStyle &vs) {
	const Sci::Line lineDoc = model.pdoc->SciLineFromPosition(pos);
	Sci::Line lineDisplay = model.pcs->DisplayFromDoc(lineDoc);
	if (surface) {
		const Sci::Position posLineStart = model.pdoc->LineStart(lineDoc);
		const int posInLine = static_cast<int>(pos - posLineStart);
		LineLayout * const ll = RetrieveLineLayout(lineDoc, model);
		LayoutLine(model, surface, vs, ll, model.wrapWidth, LayoutLineOption::AutoUpdate, posInLine);
		lineDisplay--; // To make up for first increment ahead.
		for (int subLine = 0; subLine < ll->lines; subLine++) {
			if (posInLine >= ll->LineStart(subLine)) {
				lineDisplay++;
			}
		}
	}
	return lineDisplay;
}

Sci::Position EditView::StartEndDisplayLine(Surface *surface, const EditModel &model, Sci::Position pos, bool start, const ViewStyle &vs) {
	const Sci::Line line = model.pdoc->SciLineFromPosition(pos);
	Sci::Position posRet = Sci::invalidPosition;
	if (surface) {
		const Sci::Position posLineStart = model.pdoc->LineStart(line);
		const int posInLine = static_cast<int>(pos - posLineStart);
		LineLayout * const ll = RetrieveLineLayout(line, model);
		LayoutLine(model, surface, vs, ll, model.wrapWidth, LayoutLineOption::AutoUpdate, posInLine);
		if (posInLine <= ll->maxLineLength) {
			for (int subLine = 0; subLine < ll->lines; subLine++) {
				if ((posInLine >= ll->LineStart(subLine)) &&
					(posInLine <= ll->LineStart(subLine + 1)) &&
					(posInLine <= ll->numCharsBeforeEOL)) {
					if (start) {
						posRet = ll->LineStart(subLine) + posLineStart;
					} else {
						if (subLine == ll->lines - 1)
							posRet = ll->numCharsBeforeEOL + posLineStart;
						else
							posRet = model.pdoc->MovePositionOutsideChar(ll->LineStart(subLine + 1) + posLineStart - 1, -1, false);
					}
				}
			}
		}
	}
	return posRet;
}

namespace {

constexpr ColourRGBA bugColour = ColourRGBA(0xff, 0, 0xfe, 0xf0);

// Selection background colours are always defined, the value_or is to show if bug

ColourRGBA SelectionBackground(const EditModel &model, const ViewStyle &vsDraw, InSelection inSelection) {
	if (inSelection == InSelection::inNone)
		return bugColour;	// Not selected is a bug

	Element element = Element::SelectionBack;
	if (inSelection == InSelection::inAdditional)
		element = Element::SelectionAdditionalBack;
	if (!model.primarySelection)
		element = Element::SelectionSecondaryBack;
	if (!model.hasFocus) {
		const auto colour = vsDraw.ElementColour(Element::SelectionInactiveBack);
		if (colour) {
			return *colour;
		}
	}
	return vsDraw.ElementColour(element).value_or(bugColour);
}

std::optional<ColourRGBA> SelectionForeground(const EditModel &model, const ViewStyle &vsDraw, InSelection inSelection) {
	if (inSelection == InSelection::inNone)
		return {};
	Element element = Element::SelectionText;
	if (inSelection == InSelection::inAdditional)
		element = Element::SelectionAdditionalText;
	if (!model.primarySelection)	// Secondary selection
		element = Element::SelectionSecondaryText;
	if (!model.hasFocus) {
		const auto colour = vsDraw.ElementColour(Element::SelectionInactiveText);
		if (colour) {
			return colour;
		} else {
			return {};
		}
	}
	return vsDraw.ElementColour(element);
}

ColourRGBA TextBackground(const EditModel &model, const ViewStyle &vsDraw, const LineLayout *ll,
	std::optional<ColourRGBA> background, InSelection inSelection, bool inHotspot, int styleMain, Sci::Position i) {
	if (inSelection && (vsDraw.selection.layer == Layer::Base)) {
		return SelectionBackground(model, vsDraw, inSelection).Opaque();
	}
	if ((vsDraw.edgeState == EdgeVisualStyle::Background) &&
		(i >= ll->edgeColumn) &&
		(i < ll->numCharsBeforeEOL))
		return vsDraw.theEdge.colour;
	if (inHotspot) {
		const auto colour = vsDraw.ElementColour(Element::HotSpotActiveBack);
		if (colour) {
			return colour->Opaque();
		}
	}
	if (background && (styleMain != StyleBraceLight) && (styleMain != StyleBraceBad)) {
		return *background;
	} else {
		return vsDraw.styles[styleMain].back;
	}
}

}

void EditView::DrawIndentGuide(Surface *surface, Sci::Line lineVisible, int lineHeight, XYPOSITION start, PRectangle rcSegment, bool highlight) const {
	const Point from = Point::FromInts(0, ((lineVisible & 1) & (lineHeight & 1)));
	const PRectangle rcCopyArea(start + 1, rcSegment.top,
		start + 2, rcSegment.bottom);
	surface->Copy(rcCopyArea, from,
		highlight ? *pixmapIndentGuideHighlight : *pixmapIndentGuide);
}

static void DrawTextBlob(Surface *surface, const ViewStyle &vsDraw, PRectangle rcSegment,
	std::string_view text, ColourRGBA textBack, ColourRGBA textFore, bool fillBackground) {
	if (rcSegment.Empty())
		return;
	if (fillBackground) {
		surface->FillRectangleAligned(rcSegment, Fill(textBack));
	}
	const Style &styleCtrl = vsDraw.styles[StyleControlChar];
	const Font *ctrlCharsFont = styleCtrl.font.get();
	const XYPOSITION normalCharHeight = std::ceil(styleCtrl.capitalHeight);
	PRectangle rcCChar = rcSegment;
	rcCChar.left = rcCChar.left + 1;
	rcCChar.top = rcSegment.top + vsDraw.maxAscent - normalCharHeight;
	rcCChar.bottom = rcSegment.top + vsDraw.maxAscent + 1;
	PRectangle rcCentral = rcCChar;
	rcCentral.top++;
	rcCentral.bottom--;
	surface->FillRectangleAligned(rcCentral, Fill(textFore));
	PRectangle rcChar = rcCChar;
	rcChar.left++;
	rcChar.right--;
	surface->DrawTextClippedUTF8(rcChar, ctrlCharsFont,
		rcSegment.top + vsDraw.maxAscent, text,
		textBack, textFore);
}

static void DrawCaretLineFramed(Surface *surface, const ViewStyle &vsDraw, const LineLayout *ll, PRectangle rcLine, int subLine) {
	const std::optional<ColourRGBA> caretlineBack = vsDraw.ElementColour(Element::CaretLineBack);
	if (!caretlineBack) {
		return;
 	}

	const ColourRGBA colourFrame = (vsDraw.caretLine.layer == Layer::Base) ?
		caretlineBack->Opaque() : *caretlineBack;

	const int width = vsDraw.GetFrameWidth();

	// Avoid double drawing the corners by removing the left and right sides when drawing top and bottom borders
	const PRectangle rcWithoutLeftRight = rcLine.Inset(Point(width, 0.0));

	if (subLine == 0 || ll->wrapIndent == 0 || vsDraw.caretLine.layer != Layer::Base || vsDraw.caretLine.subLine) {
		// Left
		surface->FillRectangleAligned(Side(rcLine, Edge::left, width), colourFrame);
	}
	if (subLine == 0 || vsDraw.caretLine.subLine) {
		// Top
		surface->FillRectangleAligned(Side(rcWithoutLeftRight, Edge::top, width), colourFrame);
	}
	if (subLine == ll->lines - 1 || vsDraw.caretLine.layer != Layer::Base || vsDraw.caretLine.subLine) {
		// Right
		surface->FillRectangleAligned(Side(rcLine, Edge::right, width), colourFrame);
	}
	if (subLine == ll->lines - 1 || vsDraw.caretLine.subLine) {
		// Bottom
		surface->FillRectangleAligned(Side(rcWithoutLeftRight, Edge::bottom, width), colourFrame);
	}
}

void EditView::DrawEOL(Surface *surface, const EditModel &model, const ViewStyle &vsDraw, const LineLayout *ll,
	PRectangle rcLine, Sci::Line line, Sci::Position lineEnd, int xStart, int subLine, XYACCUMULATOR subLineStart,
	std::optional<ColourRGBA> background) const {

	const Sci::Position posLineStart = model.pdoc->LineStart(line);
	PRectangle rcSegment = rcLine;

	const bool lastSubLine = subLine == (ll->lines - 1);
	XYPOSITION virtualSpace = 0;
	if (lastSubLine) {
		const XYPOSITION spaceWidth = vsDraw.styles[ll->EndLineStyle()].spaceWidth;
		virtualSpace = model.sel.VirtualSpaceFor(model.pdoc->LineEnd(line)) * spaceWidth;
	}
	const XYPOSITION xEol = static_cast<XYPOSITION>(ll->positions[lineEnd] - subLineStart);

	// Fill the virtual space and show selections within it
	if (virtualSpace > 0.0f) {
		rcSegment.left = xEol + xStart;
		rcSegment.right = xEol + xStart + virtualSpace;
		const ColourRGBA backgroundFill = background.value_or(vsDraw.styles[ll->styles[ll->numCharsInLine]].back);
		surface->FillRectangleAligned(rcSegment, backgroundFill);
		if (vsDraw.selection.visible && (vsDraw.selection.layer == Layer::Base)) {
			const SelectionSegment virtualSpaceRange(SelectionPosition(model.pdoc->LineEnd(line)),
				SelectionPosition(model.pdoc->LineEnd(line),
					model.sel.VirtualSpaceFor(model.pdoc->LineEnd(line))));
			for (size_t r = 0; r < model.sel.Count(); r++) {
				const SelectionSegment portion = model.sel.Range(r).Intersect(virtualSpaceRange);
				if (!portion.Empty()) {
					const XYPOSITION spaceWidth = vsDraw.styles[ll->EndLineStyle()].spaceWidth;
					rcSegment.left = xStart + ll->positions[portion.start.Position() - posLineStart] -
						static_cast<XYPOSITION>(subLineStart)+portion.start.VirtualSpace() * spaceWidth;
					rcSegment.right = xStart + ll->positions[portion.end.Position() - posLineStart] -
						static_cast<XYPOSITION>(subLineStart)+portion.end.VirtualSpace() * spaceWidth;
					rcSegment.left = (rcSegment.left > rcLine.left) ? rcSegment.left : rcLine.left;
					rcSegment.right = (rcSegment.right < rcLine.right) ? rcSegment.right : rcLine.right;
					surface->FillRectangleAligned(rcSegment, Fill(
						SelectionBackground(model, vsDraw, model.sel.RangeType(r)).Opaque()));
				}
			}
		}
	}

	InSelection eolInSelection = InSelection::inNone;
	if (vsDraw.selection.visible && lastSubLine) {
		eolInSelection = model.LineEndInSelection(line);
	}

	const bool drawEOLSelection = eolInSelection && (line < model.pdoc->LinesTotal() - 1);
	const ColourRGBA selectionBack = SelectionBackground(model, vsDraw, eolInSelection);

	// Draw the [CR], [LF], or [CR][LF] blobs if visible line ends are on
	XYPOSITION blobsWidth = 0;
	if (lastSubLine) {
		for (Sci::Position eolPos = ll->numCharsBeforeEOL; eolPos<ll->numCharsInLine;) {
			const int styleMain = ll->styles[eolPos];
			const std::optional<ColourRGBA> selectionFore = SelectionForeground(model, vsDraw, eolInSelection);
			ColourRGBA textFore = selectionFore.value_or(vsDraw.styles[styleMain].fore);
			char hexits[4];
			std::string_view ctrlChar;
			Sci::Position widthBytes = 1;
			RepresentationAppearance appearance = RepresentationAppearance::Blob;
			const Representation *repr = model.reprs.RepresentationFromCharacter(std::string_view(&ll->chars[eolPos], ll->numCharsInLine - eolPos));
			if (repr) {
				// Representation of whole text
				widthBytes = ll->numCharsInLine - eolPos;
			} else {
				repr = model.reprs.RepresentationFromCharacter(std::string_view(&ll->chars[eolPos], 1));
			}
			if (repr) {
				ctrlChar = repr->GetStringRep();
				appearance = repr->appearance;
				if (FlagSet(appearance, RepresentationAppearance::Colour)) {
					textFore = repr->colour;
				}
			} else {
				const unsigned char chEOL = ll->chars[eolPos];
				if (UTF8IsAscii(chEOL)) {
					ctrlChar = ControlCharacterString(chEOL);
				} else {
					hexits[0] = 'x';
					hexits[1] = "0123456789ABCDEF"[chEOL >> 4];
					hexits[2] = "0123456789ABCDEF"[chEOL & 15];
					hexits[3] = '\0';
					ctrlChar = hexits;
				}
			}

			rcSegment.left = xStart + ll->positions[eolPos] - static_cast<XYPOSITION>(subLineStart) + virtualSpace;
			rcSegment.right = xStart + ll->positions[eolPos + widthBytes] - static_cast<XYPOSITION>(subLineStart) + virtualSpace;
			blobsWidth += rcSegment.Width();
			const ColourRGBA textBack = TextBackground(model, vsDraw, ll, background, eolInSelection, false, styleMain, eolPos);
			if (drawEOLSelection) {
				if (vsDraw.selection.layer == Layer::Base) {
					surface->FillRectangleAligned(rcSegment, Fill(selectionBack.Opaque()));
				} else {
					surface->FillRectangleAligned(rcSegment, Fill(textBack));
				}
			} else {
				surface->FillRectangleAligned(rcSegment, Fill(textBack));
			}
			ColourRGBA blobText = textBack;
			if (drawEOLSelection && (vsDraw.selection.layer == Layer::UnderText)) {
				surface->FillRectangleAligned(rcSegment, selectionBack);
				//blobText = textBack.MixedWith(selectionBack, selectionBack.GetAlphaComponent());
				blobText = ColourRGBA::MixAlpha(textBack, selectionBack);
			}
			if (FlagSet(appearance, RepresentationAppearance::Blob)) {
				DrawTextBlob(surface, vsDraw, rcSegment, ctrlChar, blobText, textFore, phasesDraw == PhasesDraw::One);
			} else {
				surface->DrawTextTransparentUTF8(rcSegment, vsDraw.styles[StyleControlChar].font.get(),
					rcSegment.top + vsDraw.maxAscent, ctrlChar, textFore);
			}
			if (drawEOLSelection && (vsDraw.selection.layer == Layer::OverText)) {
				surface->FillRectangleAligned(rcSegment, selectionBack);
			}
			eolPos += widthBytes;
		}
	}

	// Draw the eol-is-selected rectangle
	rcSegment.left = xEol + xStart + virtualSpace + blobsWidth;
	rcSegment.right = rcSegment.left + vsDraw.aveCharWidth;

	if (drawEOLSelection && (vsDraw.selection.layer == Layer::Base)) {
		surface->FillRectangleAligned(rcSegment, Fill(selectionBack.Opaque()));
	} else {
		if (background) {
			surface->FillRectangleAligned(rcSegment, Fill(*background));
		} else if (line < model.pdoc->LinesTotal() - 1) {
			surface->FillRectangleAligned(rcSegment, Fill(vsDraw.styles[ll->styles[ll->numCharsInLine]].back));
		} else if (vsDraw.styles[ll->styles[ll->numCharsInLine]].eolFilled) {
			surface->FillRectangleAligned(rcSegment, Fill(vsDraw.styles[ll->styles[ll->numCharsInLine]].back));
		} else {
			surface->FillRectangleAligned(rcSegment, Fill(vsDraw.styles[StyleDefault].back));
		}
		if (drawEOLSelection && (vsDraw.selection.layer != Layer::Base)) {
			surface->FillRectangleAligned(rcSegment, selectionBack);
		}
	}

	rcSegment.left = std::max(rcSegment.right, rcLine.left);
	rcSegment.right = rcLine.right;

	const bool drawEOLAnnotationStyledText = (vsDraw.eolAnnotationVisible !=  EOLAnnotationVisible::Hidden) && model.pdoc->EOLAnnotationStyledText(line).text;
	const bool fillRemainder = (!lastSubLine || (!model.GetFoldDisplayText(line, ll->PartialPosition()) && !drawEOLAnnotationStyledText));
	if (fillRemainder) {
		// Fill the remainder of the line
		rcSegment.left -= vsDraw.aveCharWidth*(100 - vsDraw.selection.eolSelectedWidth)/100;
		FillLineRemainder(surface, model, vsDraw, ll, line, rcSegment, subLine);
	}

	bool drawWrapMarkEnd = false;

	if (subLine + 1 < ll->lines) {
		if (FlagSet(vsDraw.wrap.visualFlags, WrapVisualFlag::End)) {
			drawWrapMarkEnd = ll->LineStart(subLine + 1) != 0;
		}
		if (vsDraw.IsLineFrameOpaque(model.caret.active, ll->containsCaret)) {
			// Draw right of frame under marker
			surface->FillRectangleAligned(Side(rcLine, Edge::right, vsDraw.GetFrameWidth()),
				vsDraw.ElementColour(Element::CaretLineBack)->Opaque());
		}
	}

	if (drawWrapMarkEnd) {
		PRectangle rcPlace = rcSegment;

		if (FlagSet(vsDraw.wrap.visualFlagsLocation, WrapVisualLocation::EndByText)) {
			rcPlace.left = xEol + xStart + virtualSpace;
			rcPlace.right = rcPlace.left + vsDraw.aveCharWidth;
		} else {
			// rcLine is clipped to text area
			rcPlace.right = rcLine.right;
			rcPlace.left = rcPlace.right - vsDraw.aveCharWidth;
		}
		if (!customDrawWrapMarker) {
			DrawWrapMarker(surface, rcPlace, true, vsDraw.WrapColour());
		} else {
			customDrawWrapMarker(surface, rcPlace, true, vsDraw.WrapColour());
		}
	}
}

static void DrawIndicator(int indicNum, Sci::Position startPos, Sci::Position endPos, Surface *surface, const ViewStyle &vsDraw,
	const LineLayout *ll, int xStart, PRectangle rcLine, Sci::Position secondCharacter, int subLine, Indicator::State state,
	int value, bool bidiEnabled, int tabWidthMinimumPixels) {

	const XYPOSITION subLineStart = ll->positions[ll->LineStart(subLine)];
	const XYPOSITION horizontalOffset = xStart - subLineStart;

	std::vector<PRectangle> rectangles;

	const XYPOSITION left = ll->XInLine(startPos) + horizontalOffset;
	const XYPOSITION right = ll->XInLine(endPos) + horizontalOffset;
	const PRectangle rcIndic(left, rcLine.top + vsDraw.maxAscent, right, rcLine.top + vsDraw.maxAscent + 3);

	if (bidiEnabled) {
		ScreenLine screenLine(ll, subLine, vsDraw, rcLine.right - xStart, tabWidthMinimumPixels);
		const Range lineRange = ll->SubLineRange(subLine, LineLayout::Scope::visibleOnly);

		const std::unique_ptr<IScreenLineLayout> slLayout = surface->Layout(&screenLine);
		const std::vector<Interval> intervals = slLayout->FindRangeIntervals(
			startPos - lineRange.start, endPos - lineRange.start);
		for (const Interval &interval : intervals) {
			PRectangle rcInterval = rcIndic;
			rcInterval.left = interval.left + xStart;
			rcInterval.right = interval.right + xStart;
			rectangles.push_back(rcInterval);
		}
	} else {
		rectangles.push_back(rcIndic);
	}

	for (const PRectangle &rc : rectangles) {
		PRectangle rcFirstCharacter = rc;
		// Allow full descent space for character indicators
		rcFirstCharacter.bottom = rcLine.top + vsDraw.maxAscent + vsDraw.maxDescent;
		if (secondCharacter >= 0) {
			rcFirstCharacter.right = ll->XInLine(secondCharacter) + horizontalOffset;
		} else {
			// Indicator continued from earlier line so make an empty box and don't draw
			rcFirstCharacter.right = rcFirstCharacter.left;
		}
		vsDraw.indicators[indicNum].Draw(surface, rc, rcLine, rcFirstCharacter, state, value);
	}
}

static void DrawIndicators(Surface *surface, const EditModel &model, const ViewStyle &vsDraw, const LineLayout *ll,
	Sci::Line line, int xStart, PRectangle rcLine, int subLine, Sci::Position lineEnd, bool under, int tabWidthMinimumPixels) {
	// Draw decorators
	const Sci::Position posLineStart = model.pdoc->LineStart(line);
	const Sci::Position lineStart = ll->LineStart(subLine);
	const Sci::Position posLineEnd = posLineStart + lineEnd;

	for (const auto *deco : model.pdoc->decorations->View()) {
		if (under == vsDraw.indicators[deco->Indicator()].under) {
			Sci::Position startPos = posLineStart + lineStart;
			while (startPos < posLineEnd) {
				const Range rangeRun(deco->StartRun(startPos), deco->EndRun(startPos));
				const Sci::Position endPos = std::min(rangeRun.end, posLineEnd);
				const int value = deco->ValueAt(startPos);
				if (value) {
					const bool hover = vsDraw.indicators[deco->Indicator()].IsDynamic() &&
						rangeRun.ContainsCharacter(model.hoverIndicatorPos);
					const Indicator::State state = hover ? Indicator::State::hover : Indicator::State::normal;
					const Sci::Position posSecond = model.pdoc->MovePositionOutsideChar(rangeRun.First() + 1, 1);
					DrawIndicator(deco->Indicator(), startPos - posLineStart, endPos - posLineStart,
						surface, vsDraw, ll, xStart, rcLine, posSecond - posLineStart, subLine, state,
						value, model.BidirectionalEnabled(), tabWidthMinimumPixels);
				}
				startPos = endPos;
			}
		}
	}

	// Use indicators to highlight matching braces
	if ((vsDraw.braceHighlightIndicatorSet && (model.bracesMatchStyle == StyleBraceLight)) ||
		(vsDraw.braceBadLightIndicatorSet && (model.bracesMatchStyle == StyleBraceBad))) {
		const int braceIndicator = (model.bracesMatchStyle == StyleBraceLight) ? vsDraw.braceHighlightIndicator : vsDraw.braceBadLightIndicator;
		if (under == vsDraw.indicators[braceIndicator].under) {
			const Range rangeLine(posLineStart + lineStart, posLineEnd);
			for (size_t brace = 0; brace <= 1; brace++) {
				if (rangeLine.ContainsCharacter(model.braces[brace])) {
					const Sci::Position braceOffset = model.braces[brace] - posLineStart;
					if (braceOffset < ll->numCharsInLine) {
						const Sci::Position secondOffset = model.pdoc->MovePositionOutsideChar(model.braces[brace] + 1, 1) - posLineStart;
						DrawIndicator(braceIndicator, braceOffset, braceOffset + 1, surface, vsDraw, ll, xStart, rcLine, secondOffset,
							subLine, Indicator::State::normal, 1, model.BidirectionalEnabled(), tabWidthMinimumPixels);
					}
				}
			}
		}
	}

	if (FlagSet(model.changeHistoryOption, ChangeHistoryOption::Indicators)) {
		// Draw editions
		constexpr int indexHistory = static_cast<int>(IndicatorNumbers::HistoryRevertedToOriginInsertion);
		{
			// Draw insertions
			Sci::Position startPos = posLineStart + lineStart;
			while (startPos < posLineEnd) {
				const Range rangeRun(startPos, model.pdoc->EditionEndRun(startPos));
				const Sci::Position endPos = std::min(rangeRun.end, posLineEnd);
				const int edition = model.pdoc->EditionAt(startPos);
				if (edition != 0) {
					const int indicator = (edition - 1) * 2 + indexHistory;
					const Sci::Position posSecond = model.pdoc->MovePositionOutsideChar(rangeRun.First() + 1, 1);
					DrawIndicator(indicator, startPos - posLineStart, endPos - posLineStart,
						surface, vsDraw, ll, xStart, rcLine, posSecond - posLineStart, subLine, Indicator::State::normal,
						1, model.BidirectionalEnabled(), tabWidthMinimumPixels);
				}
				startPos = endPos;
			}
		}
		{
			// Draw deletions
			Sci::Position startPos = posLineStart + lineStart;
			while (startPos <= posLineEnd) {
				const unsigned int editions = model.pdoc->EditionDeletesAt(startPos);
				const Sci::Position posSecond = model.pdoc->MovePositionOutsideChar(startPos + 1, 1);
				for (unsigned int edition = 0; edition < 4; edition++) {
					if (editions & (1 << edition)) {
						const int indicator = edition * 2 + indexHistory + 1;
						DrawIndicator(indicator, startPos - posLineStart, posSecond - posLineStart,
							surface, vsDraw, ll, xStart, rcLine, posSecond - posLineStart, subLine, Indicator::State::normal,
							1, model.BidirectionalEnabled(), tabWidthMinimumPixels);
					}
				}
				startPos = model.pdoc->EditionNextDelete(startPos);
			}
		}
	}
}

void EditView::DrawFoldDisplayText(Surface *surface, const EditModel &model, const ViewStyle &vsDraw, const LineLayout *ll,
	Sci::Line line, int xStart, PRectangle rcLine, int subLine, XYACCUMULATOR subLineStart, DrawPhase phase) {
	const bool lastSubLine = subLine == (ll->lines - 1);
	if (!lastSubLine)
		return;

	const char *text = model.GetFoldDisplayText(line, ll->PartialPosition());
	if (!text)
		return;

	PRectangle rcSegment = rcLine;
	const std::string_view foldDisplayText(text);
	const Font *fontText = vsDraw.styles[StyleFoldDisplayText].font.get();
	constexpr int margin = 2;
	const XYPOSITION widthFoldDisplayText = surface->WidthText(fontText, foldDisplayText);

	InSelection eolInSelection = InSelection::inNone;
	if (vsDraw.selection.visible) {
		eolInSelection = model.LineEndInSelection(line);
	}

	const XYPOSITION spaceWidth = vsDraw.styles[ll->EndLineStyle()].spaceWidth;
	const XYPOSITION virtualSpace = model.sel.VirtualSpaceFor(
		model.pdoc->LineEnd(line)) * spaceWidth;
	rcSegment.left = xStart + static_cast<XYPOSITION>(ll->positions[ll->numCharsInLine] - subLineStart) + virtualSpace + vsDraw.aveCharWidth;
	rcSegment.right = rcSegment.left + widthFoldDisplayText + margin*2;

	const std::optional<ColourRGBA> background = vsDraw.Background(model.GetMark(line), model.caret.active, ll->containsCaret);
	const std::optional<ColourRGBA> selectionFore = SelectionForeground(model, vsDraw, eolInSelection);
	const ColourRGBA textFore = selectionFore.value_or(vsDraw.styles[StyleFoldDisplayText].fore);
	const ColourRGBA textBack = TextBackground(model, vsDraw, ll, background, eolInSelection,
		false, StyleFoldDisplayText, -1);

	if (model.trackLineWidth) {
		if (rcSegment.right + 1 > lineWidthMaxSeen) {
			// Fold display text border drawn on rcSegment.right with width 1 is the last visible object of the line
			lineWidthMaxSeen = static_cast<int>(rcSegment.right + 1);
		}
	}

	PRectangle rcBox = rcSegment;
	rcBox.top += 1 + vsDraw.extraAscent/2;
	rcBox.bottom -= vsDraw.extraDescent/2;
	rcBox.left = std::round(rcBox.left);
	rcBox.right = std::round(rcBox.right) + 1;

	if (FlagSet(phase, DrawPhase::back)) {
		// Fill Remainder of the line
		PRectangle rcRemainder = rcSegment;
		rcRemainder.left = std::max(rcRemainder.left, rcLine.left);
		rcRemainder.right = rcLine.right;
		FillLineRemainder(surface, model, vsDraw, ll, line, rcRemainder, subLine);

		ColourRGBA backgroundFill = textBack;
		if (vsDraw.styles[ll->styles[ll->numCharsInLine]].eolFilled) {
			const ColourRGBA eolFilled = vsDraw.styles[ll->styles[ll->numCharsInLine]].back;
			backgroundFill = AlphaBlend(backgroundFill, eolFilled, vsDraw.ElementColour(Element::SelectionBack)->GetAlpha());
		}
		surface->FillRectangleAligned(rcBox, Fill(backgroundFill));
	}

	if (FlagSet(phase, DrawPhase::text)) {
		rcSegment.left += margin;
		if (phasesDraw != PhasesDraw::One) {
			surface->DrawTextTransparent(rcSegment, fontText,
				rcSegment.top + vsDraw.maxAscent, foldDisplayText,
				textFore);
		} else {
			surface->DrawTextNoClip(rcSegment, fontText,
				rcSegment.top + vsDraw.maxAscent, foldDisplayText,
				textFore, textBack);
		}
	}

	if (FlagSet(phase, DrawPhase::indicatorsFore)) {
		if (model.foldDisplayTextStyle == FoldDisplayTextStyle::Boxed) {
			surface->RectangleFrame(rcBox, Stroke(textFore));
		}
	}

	if (FlagSet(phase, DrawPhase::selectionTranslucent)) {
		if (eolInSelection && (line < model.pdoc->LinesTotal() - 1) && (vsDraw.selection.layer != Layer::Base)) {
			surface->FillRectangleAligned(rcBox, SelectionBackground(model, vsDraw, eolInSelection));
		}
	}
}

void EditView::DrawEOLAnnotationText(Surface *surface, const EditModel &model, const ViewStyle &vsDraw, const LineLayout *ll, Sci::Line line, int xStart, PRectangle rcLine, int subLine, XYACCUMULATOR subLineStart, DrawPhase phase) {
	const bool lastSubLine = subLine == (ll->lines - 1);
	if (!lastSubLine)
		return;

	if (vsDraw.eolAnnotationVisible ==  EOLAnnotationVisible::Hidden) {
		return;
	}
	const StyledText stEOLAnnotation = model.pdoc->EOLAnnotationStyledText(line);
	if (!stEOLAnnotation.text || !ValidStyledText(vsDraw, vsDraw.eolAnnotationStyleOffset, stEOLAnnotation)) {
		return;
	}
	const std::string_view eolAnnotationText(stEOLAnnotation.text, stEOLAnnotation.length);
	const size_t style = stEOLAnnotation.style + vsDraw.eolAnnotationStyleOffset;

	PRectangle rcSegment = rcLine;
	const Font *fontText = vsDraw.styles[style].font.get();

	const Surface::Ends ends = static_cast<Surface::Ends>(static_cast<int>(vsDraw.eolAnnotationVisible) & 0xff);
	const Surface::Ends leftSide = static_cast<Surface::Ends>(static_cast<int>(ends) & 0xf);
	const Surface::Ends rightSide = static_cast<Surface::Ends>(static_cast<int>(ends) & 0xf0);

	XYPOSITION leftBoxSpace = 0;
	XYPOSITION rightBoxSpace = 0;
	if (vsDraw.eolAnnotationVisible >= EOLAnnotationVisible::Boxed) {
		leftBoxSpace = 1;
		rightBoxSpace = 1;
		if (vsDraw.eolAnnotationVisible != EOLAnnotationVisible::Boxed) {
			switch (leftSide) {
			case Surface::Ends::leftFlat:
				leftBoxSpace = 1;
				break;
			case Surface::Ends::leftAngle:
				leftBoxSpace = rcLine.Height() / 2.0;
				break;
			case Surface::Ends::semiCircles:
			default:
				leftBoxSpace = rcLine.Height() / 3.0;
			   break;
			}
			switch (rightSide) {
			case Surface::Ends::rightFlat:
				rightBoxSpace = 1;
				break;
			case Surface::Ends::rightAngle:
				rightBoxSpace = rcLine.Height() / 2.0;
				break;
			case Surface::Ends::semiCircles:
			default:
				rightBoxSpace = rcLine.Height() / 3.0;
			   break;
			}
		}
	}
	const int widthEOLAnnotationText = static_cast<int>(surface->WidthTextUTF8(fontText, eolAnnotationText) +
		leftBoxSpace + rightBoxSpace);

	const XYPOSITION spaceWidth = vsDraw.styles[ll->EndLineStyle()].spaceWidth;
	const XYPOSITION virtualSpace = model.sel.VirtualSpaceFor(
		model.pdoc->LineEnd(line)) * spaceWidth;
	rcSegment.left = xStart +
		static_cast<XYPOSITION>(ll->positions[ll->numCharsInLine] - subLineStart)
		+ virtualSpace + vsDraw.aveCharWidth;

	const char *textFoldDisplay = model.GetFoldDisplayText(line, ll->PartialPosition());
	if (textFoldDisplay) {
		const std::string_view foldDisplayText(textFoldDisplay);
		rcSegment.left += (surface->WidthText(vsDraw.styles[StyleFoldDisplayText].font.get(), foldDisplayText) + vsDraw.aveCharWidth);
	}
	rcSegment.right = rcSegment.left + widthEOLAnnotationText;

	const std::optional<ColourRGBA> background = vsDraw.Background(model.GetMark(line), model.caret.active, ll->containsCaret);
	const ColourRGBA textFore = vsDraw.styles[style].fore;
	const ColourRGBA textBack = TextBackground(model, vsDraw, ll, background, InSelection::inNone,
											false, static_cast<int>(style), -1);

	if (model.trackLineWidth) {
		if (rcSegment.right + 1> lineWidthMaxSeen) {
			// EOL Annotation text border drawn on rcSegment.right with width 1 is the last visible object of the line
			lineWidthMaxSeen = static_cast<int>(rcSegment.right + 1);
		}
	}

	if (FlagSet(phase, DrawPhase::back)) {
		// This fills in the whole remainder of the line even though
		// it may be double drawing. This is to allow stadiums with
		// curved or angled ends to have the area outside in the correct
		// background colour.
		PRectangle rcRemainder = rcSegment;
		rcRemainder.right = rcLine.right;
		FillLineRemainder(surface, model, vsDraw, ll, line, rcRemainder, subLine);
	}

	PRectangle rcText = rcSegment;
	rcText.left += leftBoxSpace;
	rcText.right -= rightBoxSpace;

	// For single phase drawing, draw the text then any box over it
	if (FlagSet(phase, DrawPhase::text)) {
		if (phasesDraw == PhasesDraw::One) {
			surface->DrawTextNoClipUTF8(rcText, fontText,
			rcText.top + vsDraw.maxAscent, eolAnnotationText,
			textFore, textBack);
		}
	}

	// Draw any box or stadium shape
	if (FlagSet(phase, DrawPhase::indicatorsBack)) {
		if (vsDraw.eolAnnotationVisible >= EOLAnnotationVisible::Boxed) {
			PRectangle rcBox = rcSegment;
			rcBox.left = std::round(rcBox.left);
			rcBox.right = std::round(rcBox.right);
			if (vsDraw.eolAnnotationVisible == EOLAnnotationVisible::Boxed) {
				surface->RectangleFrame(rcBox, Stroke(textFore));
			} else {
				if (phasesDraw == PhasesDraw::One) {
					// Draw an outline around the text
					surface->Stadium(rcBox, FillStroke(ColourRGBA(textBack, 0), textFore, 1.0), ends);
				} else {
					// Draw with a fill to fill the edges of the shape.
					surface->Stadium(rcBox, FillStroke(textBack, textFore, 1.0), ends);
				}
			}
		}
	}

	// For multi-phase drawing draw the text last as transparent over any box
	if (FlagSet(phase, DrawPhase::text)) {
		if (phasesDraw != PhasesDraw::One) {
			surface->DrawTextTransparentUTF8(rcText, fontText,
				rcText.top + vsDraw.maxAscent, eolAnnotationText,
				textFore);
		}
	}
}

static constexpr bool AnnotationBoxedOrIndented(AnnotationVisible annotationVisible) noexcept {
	return annotationVisible == AnnotationVisible::Boxed || annotationVisible == AnnotationVisible::Indented;
}

void EditView::DrawAnnotation(Surface *surface, const EditModel &model, const ViewStyle &vsDraw, const LineLayout *ll,
	Sci::Line line, int xStart, PRectangle rcLine, int subLine, DrawPhase phase) {
	const int indent = static_cast<int>(model.pdoc->GetLineIndentation(line) * vsDraw.spaceWidth);
	PRectangle rcSegment = rcLine;
	const int annotationLine = subLine - ll->lines;
	const StyledText stAnnotation = model.pdoc->AnnotationStyledText(line);
	if (stAnnotation.text && ValidStyledText(vsDraw, vsDraw.annotationStyleOffset, stAnnotation)) {
		if (FlagSet(phase, DrawPhase::back)) {
			surface->FillRectangleAligned(rcSegment, Fill(vsDraw.styles[0].back));
		}
		rcSegment.left = static_cast<XYPOSITION>(xStart);
		if (model.trackLineWidth || AnnotationBoxedOrIndented(vsDraw.annotationVisible)) {
			// Only care about calculating width if tracking or need to draw indented box
			int widthAnnotation = WidestLineWidth(surface, vsDraw, vsDraw.annotationStyleOffset, stAnnotation);
			if (AnnotationBoxedOrIndented(vsDraw.annotationVisible)) {
				widthAnnotation += static_cast<int>(vsDraw.spaceWidth * 2); // Margins
				rcSegment.left = static_cast<XYPOSITION>(xStart + indent);
				rcSegment.right = rcSegment.left + widthAnnotation;
			}
			lineWidthMaxSeen = std::max(lineWidthMaxSeen, widthAnnotation);
		}
		const int annotationLines = model.pdoc->AnnotationLines(line);
		size_t start = 0;
		size_t lengthAnnotation = stAnnotation.LineLength(start);
		int lineInAnnotation = 0;
		while ((lineInAnnotation < annotationLine) && (start < stAnnotation.length)) {
			start += lengthAnnotation + 1;
			lengthAnnotation = stAnnotation.LineLength(start);
			lineInAnnotation++;
		}
		PRectangle rcText = rcSegment;
		if ((FlagSet(phase, DrawPhase::back)) && AnnotationBoxedOrIndented(vsDraw.annotationVisible)) {
			surface->FillRectangleAligned(rcText,
				Fill(vsDraw.styles[stAnnotation.StyleAt(start) + vsDraw.annotationStyleOffset].back));
			rcText.left += vsDraw.spaceWidth;
		}
		DrawStyledText(surface, vsDraw, vsDraw.annotationStyleOffset, rcText,
			stAnnotation, start, lengthAnnotation, phase);
		if ((FlagSet(phase, DrawPhase::back)) && (vsDraw.annotationVisible == AnnotationVisible::Boxed)) {
			const ColourRGBA colourBorder = vsDraw.styles[vsDraw.annotationStyleOffset].fore;
			const PRectangle rcBorder = PixelAlignOutside(rcSegment, surface->PixelDivisions());
			surface->FillRectangle(Side(rcBorder, Edge::left, 1), colourBorder);
			surface->FillRectangle(Side(rcBorder, Edge::right, 1), colourBorder);
			if (subLine == ll->lines) {
				surface->FillRectangle(Side(rcBorder, Edge::top, 1), colourBorder);
			}
			if (subLine == ll->lines + annotationLines - 1) {
				surface->FillRectangle(Side(rcBorder, Edge::bottom, 1), colourBorder);
			}
		}
	} else {
#ifndef NDEBUG
		// No annotation to draw so show bug with bugColour
		if (FlagSet(phase, DrawPhase::back)) {
			surface->FillRectangle(rcSegment, bugColour.Opaque());
		}
#endif
	}
}

static void DrawBlockCaret(Surface *surface, const EditModel &model, const ViewStyle &vsDraw, const LineLayout *ll,
	int subLine, int xStart, Sci::Position offset, Sci::Position posCaret, PRectangle rcCaret, ColourRGBA caretColour) {

	const Sci::Position lineStart = ll->LineStart(subLine);
	Sci::Position posBefore = posCaret;
	Sci::Position posAfter = model.pdoc->MovePositionOutsideChar(posCaret + 1, 1);
	Sci::Position numCharsToDraw = posAfter - posCaret;

	// Work out where the starting and ending offsets are. We need to
	// see if the previous character shares horizontal space, such as a
	// glyph / combining character. If so we'll need to draw that too.
	Sci::Position offsetFirstChar = offset;
	Sci::Position offsetLastChar = offset + (posAfter - posCaret);
	while ((posBefore > 0) && ((offsetLastChar - numCharsToDraw) >= lineStart)) {
		if ((ll->positions[offsetLastChar] - ll->positions[offsetLastChar - numCharsToDraw]) > 0) {
			// The char does not share horizontal space
			break;
		}
		// Char shares horizontal space, update the numChars to draw
		// Update posBefore to point to the prev char
		posBefore = model.pdoc->MovePositionOutsideChar(posBefore - 1, -1);
		numCharsToDraw = posAfter - posBefore;
		offsetFirstChar = offset - (posCaret - posBefore);
	}

	// See if the next character shares horizontal space, if so we'll
	// need to draw that too.
	offsetFirstChar = std::max<Sci::Position>(offsetFirstChar, 0);
	numCharsToDraw = offsetLastChar - offsetFirstChar;
	while ((offsetLastChar < ll->LineStart(subLine + 1)) && (offsetLastChar <= ll->numCharsInLine)) {
		// Update posAfter to point to the 2nd next char, this is where
		// the next character ends, and 2nd next begins. We'll need
		// to compare these two
		posBefore = posAfter;
		posAfter = model.pdoc->MovePositionOutsideChar(posAfter + 1, 1);
		offsetLastChar = offset + (posAfter - posCaret);
		if ((ll->positions[offsetLastChar] - ll->positions[offsetLastChar - (posAfter - posBefore)]) > 0) {
			// The char does not share horizontal space
			break;
		}
		// Char shares horizontal space, update the numChars to draw
		numCharsToDraw = offsetLastChar - offsetFirstChar;
	}

	// We now know what to draw, update the caret drawing rectangle
	rcCaret.left = ll->positions[offsetFirstChar] - ll->positions[lineStart] + xStart;
	rcCaret.right = ll->positions[offsetFirstChar + numCharsToDraw] - ll->positions[lineStart] + xStart;

	// Adjust caret position to take into account any word wrapping symbols.
	if ((ll->wrapIndent != 0) && (lineStart != 0)) {
		const XYPOSITION wordWrapCharWidth = ll->wrapIndent;
		rcCaret.left += wordWrapCharWidth;
		rcCaret.right += wordWrapCharWidth;
	}

	// This character is where the caret block is, we override the colours
	// (inversed) for drawing the caret here.
	const int styleMain = ll->styles[offsetFirstChar];
	const Font *fontText = vsDraw.styles[styleMain].font.get();
	const std::string_view text(&ll->chars[offsetFirstChar], numCharsToDraw);
	surface->DrawTextClipped(rcCaret, fontText,
		rcCaret.top + vsDraw.maxAscent, text, vsDraw.styles[styleMain].back,
		caretColour);
}

void EditView::DrawCarets(Surface *surface, const EditModel &model, const ViewStyle &vsDraw, const LineLayout *ll,
	Sci::Line lineDoc, int xStart, PRectangle rcLine, int subLine) const {
	// When drag is active it is the only caret drawn
	const bool drawDrag = model.posDrag.IsValid();
	if ((!vsDraw.selection.visible || (model.sel.Count() == 1 && !ll->containsCaret)) && !drawDrag) {
		return;
	}
	const Sci::Position posLineStart = model.pdoc->LineStart(lineDoc);
	// For each selection draw
	for (size_t r = 0; (r < model.sel.Count()) || drawDrag; r++) {
		const bool mainCaret = r == model.sel.Main();
		SelectionPosition posCaret = (drawDrag ? model.posDrag : model.sel.Range(r).caret);
		if (vsDraw.DrawCaretInsideSelection(model.inOverstrike, imeCaretBlockOverride) &&
			!drawDrag && posCaret > model.sel.Range(r).anchor) {
			if (posCaret.VirtualSpace() > 0) {
				posCaret.SetVirtualSpace(posCaret.VirtualSpace() - 1);
			} else {
				const Sci::Position posBefore = model.pdoc->MovePositionOutsideChar(posCaret.Position() - 1, -1);
				if (posBefore >= posLineStart) {
					posCaret.SetPosition(posBefore);
				}
			}
		}

		const int offset = static_cast<int>(posCaret.Position() - posLineStart);
		const XYPOSITION spaceWidth = vsDraw.styles[ll->EndLineStyle()].spaceWidth;
		const XYPOSITION virtualOffset = posCaret.VirtualSpace() * spaceWidth;
		if (ll->InLine(offset, subLine) && offset <= ll->numCharsBeforeEOL) {
			const int lineStart = ll->LineStart(subLine);
			XYPOSITION xposCaret = ll->positions[offset] + virtualOffset - ll->positions[lineStart];
			if (model.BidirectionalEnabled() && (posCaret.VirtualSpace() == 0)) {
				// Get caret point
				const ScreenLine screenLine(ll, subLine, vsDraw, rcLine.right, tabWidthMinimumPixels);

				const int caretPosition = offset - lineStart;

				const std::unique_ptr<IScreenLineLayout> slLayout = surface->Layout(&screenLine);
				const XYPOSITION caretLeft = slLayout->XFromPosition(caretPosition);

				// In case of start of line, the cursor should be at the right
				xposCaret = caretLeft + virtualOffset;
			}
			if (ll->wrapIndent != 0) {
				if (lineStart != 0)	// Wrapped
					xposCaret += ll->wrapIndent;
			}
			const bool caretBlinkState = (model.caret.active && model.caret.on) || (!additionalCaretsBlink && !mainCaret);
			const bool caretVisibleState = additionalCaretsVisible || mainCaret;
			if ((xposCaret >= 0) && vsDraw.IsCaretVisible(mainCaret) &&
				(drawDrag || (caretBlinkState && caretVisibleState))) {
				bool canDrawBlockCaret = true;
				bool drawBlockCaret = false;
				XYPOSITION widthOverstrikeCaret = 0;
				XYPOSITION caretWidthOffset = 0;
				PRectangle rcCaret = rcLine;

				const ViewStyle::CaretShape caretShape = vsDraw.CaretShapeForMode(model.inOverstrike, mainCaret, drawDrag, drawOverstrikeCaret, imeCaretBlockOverride);
				if (caretShape != ViewStyle::CaretShape::line) {
					if (posCaret.Position() == model.pdoc->LengthNoExcept()) {   // At end of document
						canDrawBlockCaret = false;
						widthOverstrikeCaret = vsDraw.aveCharWidth;
					} else if ((posCaret.Position() - posLineStart) >= ll->numCharsInLine) {	// At end of line
						canDrawBlockCaret = false;
						widthOverstrikeCaret = vsDraw.aveCharWidth;
					} else {
						bool invalidByte = false;
						const int widthChar = model.pdoc->LenChar(posCaret.Position(), &invalidByte);
						canDrawBlockCaret = !invalidByte;
						widthOverstrikeCaret = ll->positions[offset + widthChar] - ll->positions[offset];
					}
					// Make sure its visible
					widthOverstrikeCaret = std::max(widthOverstrikeCaret, 3.0);
				}

				if (xposCaret > 0) {
					caretWidthOffset = 0.51f;	// Move back so overlaps both character cells.
				}
				xposCaret += xStart;
				if (caretShape == ViewStyle::CaretShape::bar) {
					/* Modified bar caret */
					rcCaret.top = rcCaret.bottom - 2;
					rcCaret.left = xposCaret + 1;
					rcCaret.right = rcCaret.left + widthOverstrikeCaret - 1;
				} else if (caretShape == ViewStyle::CaretShape::block) {
					/* Block caret */
					rcCaret.left = xposCaret;
					if (canDrawBlockCaret && !IsControlCharacter(ll->chars[offset])) {
						drawBlockCaret = true;
						rcCaret.right = xposCaret + widthOverstrikeCaret;
					} else {
						rcCaret.right = xposCaret + vsDraw.aveCharWidth;
					}
				} else {
					/* Line caret */
					rcCaret.left = std::round(xposCaret - caretWidthOffset);
					rcCaret.right = rcCaret.left + vsDraw.caret.width;
				}
				const Element elementCaret = mainCaret ? Element::Caret : Element::CaretAdditional;
				const ColourRGBA caretColour = *vsDraw.ElementColour(elementCaret);
				//assert(caretColour.IsOpaque());
				if (drawBlockCaret) {
					DrawBlockCaret(surface, model, vsDraw, ll, subLine, xStart, offset, posCaret.Position(), rcCaret, caretColour);
				} else {
					surface->FillRectangleAligned(rcCaret, Fill(caretColour));
				}
			}
		}
		if (drawDrag)
			break;
	}
}

namespace {

void DrawWrapIndentAndMarker(Surface *surface, const ViewStyle &vsDraw, const LineLayout *ll,
	int xStart, PRectangle rcLine, std::optional<ColourRGBA> background, DrawWrapMarkerFn customDrawWrapMarker,
	bool caretActive) {
	// default background here..
	surface->FillRectangleAligned(rcLine, Fill(background ? *background :
		vsDraw.styles[StyleDefault].back));

	if (vsDraw.IsLineFrameOpaque(caretActive, ll->containsCaret)) {
		// Draw left of frame under marker
		surface->FillRectangleAligned(Side(rcLine, Edge::left, vsDraw.GetFrameWidth()),
			vsDraw.ElementColour(Element::CaretLineBack)->Opaque());
	}

	if (FlagSet(vsDraw.wrap.visualFlags, WrapVisualFlag::Start)) {

		// draw continuation rect
		PRectangle rcPlace = rcLine;

		rcPlace.left = static_cast<XYPOSITION>(xStart);
		rcPlace.right = rcPlace.left + ll->wrapIndent;

		if (FlagSet(vsDraw.wrap.visualFlagsLocation, WrapVisualLocation::StartByText))
			rcPlace.left = rcPlace.right - vsDraw.aveCharWidth;
		else
			rcPlace.right = rcPlace.left + vsDraw.aveCharWidth;

		if (!customDrawWrapMarker) {
			DrawWrapMarker(surface, rcPlace, false, vsDraw.WrapColour());
		} else {
			customDrawWrapMarker(surface, rcPlace, false, vsDraw.WrapColour());
		}
	}
}

// On the curses platform, the terminal is drawing its own caret, so if the caret is within
// the main selection, do not draw the selection at that position.
// Use iDoc from DrawBackground and DrawForeground here because TextSegment has been adjusted
// such that, if the caret is inside the main selection, the beginning or end of that selection
// is at the end of a text segment.
// This function should only be called if iDoc is within the main selection.
InSelection CharacterInCursesSelection(Sci::Position iDoc, const EditModel &model, const ViewStyle &vsDraw) noexcept {
	const SelectionPosition &posCaret = model.sel.RangeMain().caret;
	const bool caretAtStart = posCaret < model.sel.RangeMain().anchor && posCaret.Position() == iDoc;
	const bool caretAtEnd = posCaret > model.sel.RangeMain().anchor &&
		vsDraw.DrawCaretInsideSelection(false, false) &&
		model.pdoc->MovePositionOutsideChar(posCaret.Position() - 1, -1) == iDoc;
	return (caretAtStart || caretAtEnd) ? InSelection::inNone : InSelection::inMain;
}

}

void EditView::DrawBackground(Surface *surface, const EditModel &model, const ViewStyle &vsDraw, const LineLayout *ll,
	PRectangle rcLine, Range lineRange, Sci::Position posLineStart, int xStart,
	int subLine, std::optional<ColourRGBA> background) {

	const bool selBackDrawn = vsDraw.SelectionBackgroundDrawn();
	bool inIndentation = subLine == 0;	// Do not handle indentation except on first subline.
	const XYACCUMULATOR subLineStart = ll->positions[lineRange.start];
	// Does not take margin into account but not significant
	const XYPOSITION xStartVisible = static_cast<XYPOSITION>(subLineStart - xStart);

	const BreakFinder::BreakFor breakFor = selBackDrawn ? BreakFinder::BreakFor::Selection : BreakFinder::BreakFor::Text;
	BreakFinder bfBack(ll, &model.sel, lineRange, posLineStart, xStartVisible, breakFor, model, &vsDraw, 0);

	const bool drawWhitespaceBackground = vsDraw.WhitespaceBackgroundDrawn() && !background;

	// Background drawing loop
	while (bfBack.More()) {

		const TextSegment ts = bfBack.Next();
		const int i = ts.end() - 1;
		const Sci::Position iDoc = i + posLineStart;

		PRectangle rcSegment = rcLine;
		rcSegment.left = ll->positions[ts.start] + xStart - static_cast<XYPOSITION>(subLineStart);
		rcSegment.right = ll->positions[i + 1] + xStart - static_cast<XYPOSITION>(subLineStart);
		// Only try to draw if really visible - enhances performance by not calling environment to
		// draw strings that are completely past the right side of the window.
		if (!rcSegment.Empty() && rcSegment.Intersects(rcLine)) {
			// Clip to line rectangle, since may have a huge position which will not work with some platforms
			rcSegment.left = std::max(rcSegment.left, rcLine.left);
			rcSegment.right = std::min(rcSegment.right, rcLine.right);

			InSelection inSelection = vsDraw.selection.visible ? model.sel.CharacterInSelection(iDoc) : InSelection::inNone;
			if (FlagSet(vsDraw.caret.style, CaretStyle::Curses) && (inSelection == InSelection::inMain))
				inSelection = CharacterInCursesSelection(iDoc, model, vsDraw);
			const bool inHotspot = model.hotspot.Valid() && model.hotspot.ContainsCharacter(iDoc);
			ColourRGBA textBack = TextBackground(model, vsDraw, ll, background, inSelection,
				inHotspot, ll->styles[i], i);
			if (ts.representation) {
				if (ll->chars[i] == '\t') {
					// Tab display
					if (drawWhitespaceBackground && vsDraw.WhiteSpaceVisible(inIndentation)) {
						textBack = vsDraw.ElementColour(Element::WhiteSpaceBack)->Opaque();
					}
				} else {
					// Blob display
					inIndentation = false;
				}
				surface->FillRectangleAligned(rcSegment, Fill(textBack));
			} else {
				// Normal text display
				surface->FillRectangleAligned(rcSegment, Fill(textBack));
				if (vsDraw.viewWhitespace != WhiteSpace::Invisible) {
					for (int cpos = 0; cpos <= i - ts.start; cpos++) {
						if (ll->chars[cpos + ts.start] == ' ') {
							if (drawWhitespaceBackground && vsDraw.WhiteSpaceVisible(inIndentation)) {
								const PRectangle rcSpace(
									ll->positions[cpos + ts.start] + xStart - static_cast<XYPOSITION>(subLineStart),
									rcSegment.top,
									ll->positions[cpos + ts.start + 1] + xStart - static_cast<XYPOSITION>(subLineStart),
									rcSegment.bottom);
								surface->FillRectangleAligned(rcSpace,
									vsDraw.ElementColour(Element::WhiteSpaceBack)->Opaque());
							}
						} else {
							inIndentation = false;
						}
					}
				}
			}
		} else if (rcSegment.left > rcLine.right) {
			break;
		}
	}
}

static void DrawEdgeLine(Surface *surface, const ViewStyle &vsDraw, const LineLayout *ll, PRectangle rcLine,
	Range lineRange, int xStart) {
	if (vsDraw.edgeState == EdgeVisualStyle::Line) {
		PRectangle rcSegment = rcLine;
		const int edgeX = static_cast<int>(vsDraw.theEdge.column * vsDraw.aveCharWidth);
		rcSegment.left = static_cast<XYPOSITION>(edgeX + xStart);
		if ((ll->wrapIndent != 0) && (lineRange.start != 0))
			rcSegment.left -= ll->wrapIndent;
		rcSegment.right = rcSegment.left + 1;
		surface->FillRectangleAligned(rcSegment, Fill(vsDraw.theEdge.colour));
	} else if (vsDraw.edgeState == EdgeVisualStyle::MultiLine) {
		for (size_t edge = 0; edge < vsDraw.theMultiEdge.size(); edge++) {
			if (vsDraw.theMultiEdge[edge].column >= 0) {
				PRectangle rcSegment = rcLine;
				const int edgeX = static_cast<int>(vsDraw.theMultiEdge[edge].column * vsDraw.aveCharWidth);
				rcSegment.left = static_cast<XYPOSITION>(edgeX + xStart);
				if ((ll->wrapIndent != 0) && (lineRange.start != 0))
					rcSegment.left -= ll->wrapIndent;
				rcSegment.right = rcSegment.left + 1;
				surface->FillRectangleAligned(rcSegment, Fill(vsDraw.theMultiEdge[edge].colour));
			}
		}
	}
}

// Draw underline mark as part of background if on base layer
static void DrawMarkUnderline(Surface *surface, const EditModel &model, const ViewStyle &vsDraw,
	Sci::Line line, PRectangle rcLine) {
	MarkerMask marks = model.GetMark(line);
	for (int markBit = 0; (markBit < MarkerBitCount) && marks; markBit++) {
		if ((marks & 1) && (vsDraw.markers[markBit].markType == MarkerSymbol::Underline) &&
			(vsDraw.markers[markBit].layer == Layer::Base)) {
			PRectangle rcUnderline = rcLine;
			rcUnderline.top = rcUnderline.bottom - 2;
			surface->FillRectangleAligned(rcUnderline, Fill(vsDraw.markers[markBit].back));
		}
		marks >>= 1;
	}
}

static void DrawTranslucentSelection(Surface *surface, const EditModel &model, const ViewStyle &vsDraw, const LineLayout *ll,
	Sci::Line line, PRectangle rcLine, int subLine, Range lineRange, int xStart, int tabWidthMinimumPixels, Layer layer) {
	if (vsDraw.selection.layer == layer) {
		const Sci::Position posLineStart = model.pdoc->LineStart(line);
		const XYACCUMULATOR subLineStart = ll->positions[lineRange.start];
		// For each selection draw
		Sci::Position virtualSpaces = 0;
		if (subLine == (ll->lines - 1)) {
			virtualSpaces = model.sel.VirtualSpaceFor(model.pdoc->LineEnd(line));
		}
		const SelectionPosition posStart(posLineStart + lineRange.start);
		const SelectionPosition posEnd(posLineStart + lineRange.end, virtualSpaces);
		const SelectionSegment virtualSpaceRange(posStart, posEnd);
		for (size_t r = 0; r < model.sel.Count(); r++) {
			const SelectionSegment portion = model.sel.Range(r).Intersect(virtualSpaceRange);
			if (!portion.Empty()) {
				const ColourRGBA selectionBack = SelectionBackground(model, vsDraw, model.sel.RangeType(r));
				const XYPOSITION spaceWidth = vsDraw.styles[ll->EndLineStyle()].spaceWidth;
				if (model.BidirectionalEnabled()) {
					const Sci::Position selectionStart = portion.start.Position() - posLineStart - lineRange.start;
					const Sci::Position selectionEnd = portion.end.Position() - posLineStart - lineRange.start;

					const ScreenLine screenLine(ll, subLine, vsDraw, rcLine.right, tabWidthMinimumPixels);
					const std::unique_ptr<IScreenLineLayout> slLayout = surface->Layout(&screenLine);

					if (slLayout) {
						const std::vector<Interval> intervals = slLayout->FindRangeIntervals(selectionStart, selectionEnd);
						for (const Interval &interval : intervals) {
							const XYPOSITION rcRight = interval.right + xStart;
							const XYPOSITION rcLeft = interval.left + xStart;
							const PRectangle rcSelection(rcLeft, rcLine.top, rcRight, rcLine.bottom);
							surface->FillRectangleAligned(rcSelection, selectionBack);
						}
					}

					if (portion.end.VirtualSpace()) {
						const XYPOSITION xStartVirtual = ll->positions[lineRange.end] -
							static_cast<XYPOSITION>(subLineStart) + xStart;
						PRectangle rcSegment = rcLine;
						rcSegment.left = xStartVirtual + portion.start.VirtualSpace() * spaceWidth;
						rcSegment.right = xStartVirtual + portion.end.VirtualSpace() * spaceWidth;
						surface->FillRectangleAligned(rcSegment, selectionBack);
					}
				} else {
					PRectangle rcSegment = rcLine;
					rcSegment.left = xStart + ll->positions[portion.start.Position() - posLineStart] -
						static_cast<XYPOSITION>(subLineStart) + portion.start.VirtualSpace() * spaceWidth;
					rcSegment.right = xStart + ll->positions[portion.end.Position() - posLineStart] -
						static_cast<XYPOSITION>(subLineStart) + portion.end.VirtualSpace() * spaceWidth;
					if ((ll->wrapIndent != 0) && (lineRange.start != 0)) {
						if ((portion.start.Position() - posLineStart) == lineRange.start && model.sel.Range(r).ContainsCharacter(portion.start.Position() - 1))
							rcSegment.left -= static_cast<int>(ll->wrapIndent); // indentation added to xStart was truncated to int, so we do the same here
					}
					rcSegment.left = std::max(rcSegment.left, rcLine.left);
					rcSegment.right = std::min(rcSegment.right, rcLine.right);
					if (rcSegment.right > rcLine.left)
						surface->FillRectangleAligned(rcSegment, selectionBack);
				}
			}
		}
	}
}

// Draw any translucent whole line states
static void DrawTranslucentLineState(Surface *surface, const EditModel &model, const ViewStyle &vsDraw, const LineLayout *ll,
	Sci::Line line, PRectangle rcLine, int subLine, Layer layer) {
	if (ll->containsCaret && vsDraw.caretLine.layer == layer
		&& (model.caret.active || vsDraw.caretLine.alwaysShow) && vsDraw.ElementIsSet(Element::CaretLineBack)) {
		if (vsDraw.caretLine.frame) {
			DrawCaretLineFramed(surface, vsDraw, ll, rcLine, subLine);
		} else {
			surface->FillRectangleAligned(rcLine, *vsDraw.ElementColour(Element::CaretLineBack));
		}
	}

	const MarkerMask marksOfLine = model.GetMark(line);
	MarkerMask marksDrawnInText = marksOfLine & vsDraw.maskDrawInText;
	for (int markBit = 0; (markBit < MarkerBitCount) && marksDrawnInText; markBit++) {
		if ((marksDrawnInText & 1) && (vsDraw.markers[markBit].layer == layer)) {
			if (vsDraw.markers[markBit].markType == MarkerSymbol::Background) {
				surface->FillRectangleAligned(rcLine, vsDraw.markers[markBit].back);
			} else if (vsDraw.markers[markBit].markType == MarkerSymbol::Underline) {
				PRectangle rcUnderline = rcLine;
				rcUnderline.top = rcUnderline.bottom - 2;
				surface->FillRectangleAligned(rcUnderline, vsDraw.markers[markBit].back);
			}
		}
		marksDrawnInText >>= 1;
	}
	MarkerMask marksDrawnInLine = marksOfLine & vsDraw.maskInLine;
	for (int markBit = 0; (markBit < MarkerBitCount) && marksDrawnInLine; markBit++) {
		if ((marksDrawnInLine & 1) && (vsDraw.markers[markBit].layer == layer)) {
			surface->FillRectangleAligned(rcLine, vsDraw.markers[markBit].back);
		}
		marksDrawnInLine >>= 1;
	}
}

void EditView::DrawForeground(Surface *surface, const EditModel &model, const ViewStyle &vsDraw, const LineLayout *ll,
	Sci::Line lineVisible, PRectangle rcLine, Range lineRange, Sci::Position posLineStart, int xStart,
	int subLine, std::optional<ColourRGBA> background) const {

	const bool selBackDrawn = vsDraw.SelectionBackgroundDrawn();
	const bool drawWhitespaceBackground = vsDraw.WhitespaceBackgroundDrawn() && !background;
	bool inIndentation = subLine == 0;	// Do not handle indentation except on first subline.

	const XYACCUMULATOR subLineStart = ll->positions[lineRange.start];
	const XYPOSITION indentWidth = model.pdoc->IndentSize() * vsDraw.aveCharWidth;

	// Does not take margin into account but not significant
	const XYPOSITION xStartVisible = static_cast<XYPOSITION>(subLineStart - xStart);

	// Foreground drawing loop
	const BreakFinder::BreakFor breakFor = (((phasesDraw == PhasesDraw::One) && selBackDrawn) || vsDraw.SelectionTextDrawn())
		? BreakFinder::BreakFor::ForegroundAndSelection : BreakFinder::BreakFor::Foreground;
	BreakFinder bfFore(ll, &model.sel, lineRange, posLineStart, xStartVisible, breakFor, model, &vsDraw, 0);

	while (bfFore.More()) {

		const TextSegment ts = bfFore.Next();
		const int i = ts.end() - 1;
		const Sci::Position iDoc = i + posLineStart;

		PRectangle rcSegment = rcLine;
		rcSegment.left = ll->positions[ts.start] + xStart - static_cast<XYPOSITION>(subLineStart);
		rcSegment.right = ll->positions[i + 1] + xStart - static_cast<XYPOSITION>(subLineStart);
		// Only try to draw if really visible - enhances performance by not calling environment to
		// draw strings that are completely past the right side of the window.
		if (rcSegment.Intersects(rcLine)) {
			const int styleMain = ll->styles[i];
			ColourRGBA textFore = vsDraw.styles[styleMain].fore;
			const Font *textFont = vsDraw.styles[styleMain].font.get();
			// Hot-spot foreground
			const bool inHotspot = model.hotspot.Valid() && model.hotspot.ContainsCharacter(iDoc);
			if (inHotspot) {
				const auto colour = vsDraw.ElementColour(Element::HotSpotActive);
				if (colour)
					textFore = *colour;
			}
			if (vsDraw.indicatorsSetFore) {
				// At least one indicator sets the text colour so see if it applies to this segment
				for (const auto *deco : model.pdoc->decorations->View()) {
					const int indicatorValue = deco->ValueAt(ts.start + posLineStart);
					if (indicatorValue) {
						const Indicator &indicator = vsDraw.indicators[deco->Indicator()];
						bool hover = false;
						if (indicator.IsDynamic()) {
							const Sci::Position startPos = ts.start + posLineStart;
							const Range rangeRun(deco->StartRun(startPos), deco->EndRun(startPos));
							hover =	rangeRun.ContainsCharacter(model.hoverIndicatorPos);
						}
						if (hover) {
							if (indicator.sacHover.style == IndicatorStyle::TextFore) {
								textFore = indicator.sacHover.fore;
							}
						} else {
							if (indicator.sacNormal.style == IndicatorStyle::TextFore) {
								if (FlagSet(indicator.Flags(), IndicFlag::ValueFore))
									textFore = ColourRGBA::FromRGB(indicatorValue & static_cast<int>(IndicValue::Mask));
								else
									textFore = indicator.sacNormal.fore;
							}
						}
					}
				}
			}
			InSelection inSelection = vsDraw.selection.visible ? model.sel.CharacterInSelection(iDoc) : InSelection::inNone;
			if (FlagSet(vsDraw.caret.style, CaretStyle::Curses) && (inSelection == InSelection::inMain))
				inSelection = CharacterInCursesSelection(iDoc, model, vsDraw);
			const std::optional<ColourRGBA> selectionFore = SelectionForeground(model, vsDraw, inSelection);
			if (selectionFore) {
				textFore = *selectionFore;
			}
			ColourRGBA textBack = TextBackground(model, vsDraw, ll, background, inSelection, inHotspot, styleMain, i);
			if (ts.representation) {
				if (ll->chars[i] == '\t') {
					// Tab display
					if (phasesDraw == PhasesDraw::One) {
						if (drawWhitespaceBackground && vsDraw.WhiteSpaceVisible(inIndentation))
							textBack = vsDraw.ElementColour(Element::WhiteSpaceBack)->Opaque();
						surface->FillRectangleAligned(rcSegment, Fill(textBack));
					}
					if (inIndentation && vsDraw.viewIndentationGuides == IndentView::Real) {
						for (int indentCount = static_cast<int>((ll->positions[i] + epsilon) / indentWidth);
							indentCount <= (ll->positions[i + 1] - epsilon) / indentWidth;
							indentCount++) {
							if (indentCount > 0) {
								const XYPOSITION xIndent = std::floor(indentCount * indentWidth);
								DrawIndentGuide(surface, lineVisible, vsDraw.lineHeight, xIndent + xStart, rcSegment,
									(ll->xHighlightGuide == xIndent));
							}
						}
					}
					if (vsDraw.viewWhitespace != WhiteSpace::Invisible) {
						if (vsDraw.WhiteSpaceVisible(inIndentation)) {
							const PRectangle rcTab(rcSegment.left + 1, rcSegment.top + tabArrowHeight,
								rcSegment.right - 1, rcSegment.bottom - vsDraw.maxDescent);
							const int segmentTop = static_cast<int>(rcSegment.top) + vsDraw.lineHeight / 2;
							const ColourRGBA whiteSpaceFore = vsDraw.ElementColour(Element::WhiteSpace).value_or(textFore);
							if (!customDrawTabArrow)
								DrawTabArrow(surface, rcTab, segmentTop, vsDraw, Stroke(whiteSpaceFore, 1.0f));
							else
								customDrawTabArrow(surface, rcTab, segmentTop, vsDraw, Stroke(whiteSpaceFore, 1.0f));
						}
					}
				} else {
					inIndentation = false;
					if (vsDraw.controlCharSymbol >= 32) {
						// Using one font for all control characters so it can be controlled independently to ensure
						// the box goes around the characters tightly. Seems to be no way to work out what height
						// is taken by an individual character - internal leading gives varying results.
						const Font *ctrlCharsFont = vsDraw.styles[StyleControlChar].font.get();
						const char cc[2] = { static_cast<char>(vsDraw.controlCharSymbol), '\0' };
						surface->DrawTextNoClip(rcSegment, ctrlCharsFont,
							rcSegment.top + vsDraw.maxAscent,
							cc, textBack, textFore);
					} else {
						const std::string_view stringRep = ts.representation->GetStringRep();
						if (FlagSet(ts.representation->appearance, RepresentationAppearance::Colour)) {
							textFore = ts.representation->colour;
						}
						if (FlagSet(ts.representation->appearance, RepresentationAppearance::Blob)) {
							DrawTextBlob(surface, vsDraw, rcSegment, stringRep,
								textBack, textFore, phasesDraw == PhasesDraw::One);
						} else {
							surface->DrawTextTransparentUTF8(rcSegment, vsDraw.styles[StyleControlChar].font.get(),
								rcSegment.top + vsDraw.maxAscent, stringRep, textFore);
						}
					}
				}
			} else {
				// Normal text display
				if (vsDraw.styles[styleMain].visible) {
					const std::string_view text(&ll->chars[ts.start], i - ts.start + 1);
					if (phasesDraw != PhasesDraw::One) {
						surface->DrawTextTransparent(rcSegment, textFont,
							rcSegment.top + vsDraw.maxAscent, text, textFore);
					} else {
						surface->DrawTextNoClip(rcSegment, textFont,
							rcSegment.top + vsDraw.maxAscent, text, textFore, textBack);
					}
				}
				if (vsDraw.viewWhitespace != WhiteSpace::Invisible ||
					(inIndentation && vsDraw.viewIndentationGuides != IndentView::None)) {
					for (int cpos = 0; cpos <= i - ts.start; cpos++) {
						if (ll->chars[cpos + ts.start] == ' ') {
							if (vsDraw.viewWhitespace != WhiteSpace::Invisible) {
								if (vsDraw.WhiteSpaceVisible(inIndentation)) {
									const XYPOSITION xmid = (ll->positions[cpos + ts.start] + ll->positions[cpos + ts.start + 1]) / 2;
									if ((phasesDraw == PhasesDraw::One) && drawWhitespaceBackground) {
										textBack = vsDraw.ElementColour(Element::WhiteSpaceBack)->Opaque();
										const PRectangle rcSpace(
											ll->positions[cpos + ts.start] + xStart - static_cast<XYPOSITION>(subLineStart),
											rcSegment.top,
											ll->positions[cpos + ts.start + 1] + xStart - static_cast<XYPOSITION>(subLineStart),
											rcSegment.bottom);
										surface->FillRectangleAligned(rcSpace, Fill(textBack));
									}
									const int halfDotWidth = vsDraw.whitespaceSize / 2;
									PRectangle rcDot(xmid + xStart - halfDotWidth - static_cast<XYPOSITION>(subLineStart),
										rcSegment.top + vsDraw.lineHeight / 2, 0.0f, 0.0f);
									rcDot.right = rcDot.left + vsDraw.whitespaceSize;
									rcDot.bottom = rcDot.top + vsDraw.whitespaceSize;
									const ColourRGBA whiteSpaceFore = vsDraw.ElementColour(Element::WhiteSpace).value_or(textFore);
									surface->FillRectangleAligned(rcDot, Fill(whiteSpaceFore));
								}
							}
							if (inIndentation && vsDraw.viewIndentationGuides == IndentView::Real) {
								for (int indentCount = static_cast<int>((ll->positions[cpos + ts.start] + epsilon) / indentWidth);
									indentCount <= (ll->positions[cpos + ts.start + 1] - epsilon) / indentWidth;
									indentCount++) {
									if (indentCount > 0) {
										const XYPOSITION xIndent = std::floor(indentCount * indentWidth);
										DrawIndentGuide(surface, lineVisible, vsDraw.lineHeight, xIndent + xStart, rcSegment,
											(ll->xHighlightGuide == xIndent));
									}
								}
							}
						} else {
							inIndentation = false;
						}
					}
				}
			}
			if (inHotspot && vsDraw.hotspotUnderline) {
				PRectangle rcUL = rcSegment;
				rcUL.top = rcUL.top + vsDraw.maxAscent + 1;
				rcUL.bottom = rcUL.top + 1;
				const auto colour = vsDraw.ElementColour(Element::HotSpotActive);
				if (colour)
					surface->FillRectangleAligned(rcUL, Fill(*colour));
				else
					surface->FillRectangleAligned(rcUL, Fill(textFore));
			} else if (vsDraw.styles[styleMain].underline) {
				PRectangle rcUL = rcSegment;
				rcUL.top = rcUL.top + vsDraw.maxAscent + 1;
				rcUL.bottom = rcUL.top + 1;
				surface->FillRectangleAligned(rcUL, Fill(textFore));
			} else if (vsDraw.styles[styleMain].strike) { // 2011-12-20
				PRectangle rcUL = rcSegment;
				rcUL.top = rcUL.top + std::ceil((rcUL.bottom - rcUL.top) / 2);
				rcUL.bottom = rcUL.top + 1;
				surface->FillRectangleAligned(rcUL, Fill(textFore));
			} else if (vsDraw.styles[styleMain].overline) {// 2022-02-06
				PRectangle rcUL = rcSegment;
				rcUL.top = rcUL.top + 1;
				rcUL.bottom = rcUL.top + 1;
				surface->FillRectangleAligned(rcUL, Fill(textFore));
			}
		} else if (rcSegment.left > rcLine.right) {
			break;
		}
	}
}

void EditView::DrawIndentGuidesOverEmpty(Surface *surface, const EditModel &model, const ViewStyle &vsDraw, const LineLayout *ll,
	Sci::Line line, Sci::Line lineVisible, PRectangle rcLine, int xStart, int subLine) const {
	if ((vsDraw.viewIndentationGuides == IndentView::LookForward || vsDraw.viewIndentationGuides == IndentView::LookBoth)
		&& (subLine == 0)) {
		const Sci::Position posLineStart = model.pdoc->LineStart(line);
		int indentSpace = model.pdoc->GetLineIndentation(line);
		int xStartText = static_cast<int>(ll->positions[model.pdoc->GetLineIndentPosition(line) - posLineStart]);

		// Find the most recent line with some text

		Sci::Line lineLastWithText = line;
		while (lineLastWithText > std::max(line - 20, static_cast<Sci::Line>(0)) && model.pdoc->IsWhiteLine(lineLastWithText)) {
			lineLastWithText--;
		}
		if (lineLastWithText < line) {
			xStartText = 100000;	// Don't limit to visible indentation on empty line
			// This line is empty, so use indentation of last line with text
			int indentLastWithText = model.pdoc->GetLineIndentation(lineLastWithText);
			const int isFoldHeader = LevelIsHeader(model.pdoc->GetFoldLevel(lineLastWithText));
			if (isFoldHeader) {
				// Level is one more level than parent
				indentLastWithText += model.pdoc->IndentSize();
			}
			if (vsDraw.viewIndentationGuides == IndentView::LookForward) {
				// In viLookForward mode, previous line only used if it is a fold header
				if (isFoldHeader) {
					indentSpace = std::max(indentSpace, indentLastWithText);
				}
			} else {	// viLookBoth
				indentSpace = std::max(indentSpace, indentLastWithText);
			}
		}

		Sci::Line lineNextWithText = line;
		while (lineNextWithText < std::min(line + 20, model.pdoc->LinesTotal()) && model.pdoc->IsWhiteLine(lineNextWithText)) {
			lineNextWithText++;
		}
		if (lineNextWithText > line) {
			xStartText = 100000;	// Don't limit to visible indentation on empty line
			// This line is empty, so use indentation of first next line with text
			indentSpace = std::max(indentSpace,
				model.pdoc->GetLineIndentation(lineNextWithText));
		}

		for (int indentPos = model.pdoc->IndentSize(); indentPos < indentSpace; indentPos += model.pdoc->IndentSize()) {
			const XYPOSITION xIndent = std::floor(indentPos * vsDraw.aveCharWidth);
			if (xIndent < xStartText) {
				DrawIndentGuide(surface, lineVisible, vsDraw.lineHeight, xIndent + xStart, rcLine,
					(ll->xHighlightGuide == xIndent));
			}
		}
	}
}

void EditView::DrawLine(Surface *surface, const EditModel &model, const ViewStyle &vsDraw, const LineLayout *ll,
	Sci::Line line, Sci::Line lineVisible, int xStart, PRectangle rcLine, int subLine, DrawPhase phase) {

	if (subLine >= ll->lines) {
		DrawAnnotation(surface, model, vsDraw, ll, line, xStart, rcLine, subLine, phase);
		return; // No further drawing
	}

	// See if something overrides the line background colour.
	const std::optional<ColourRGBA> background = vsDraw.Background(model.GetMark(line), model.caret.active, ll->containsCaret);

	const Sci::Position posLineStart = model.pdoc->LineStart(line);

	const Range lineRange = ll->SubLineRange(subLine, LineLayout::Scope::visibleOnly);
	const Range lineRangeIncludingEnd = ll->SubLineRange(subLine, LineLayout::Scope::includeEnd);
	const XYACCUMULATOR subLineStart = ll->positions[lineRange.start];

	if ((ll->wrapIndent != 0) && (subLine > 0)) {
		if (FlagSet(phase, DrawPhase::back)) {
			DrawWrapIndentAndMarker(surface, vsDraw, ll, xStart, rcLine, background, customDrawWrapMarker, model.caret.active);
		}
		xStart += static_cast<int>(ll->wrapIndent);
	}

	if (phasesDraw != PhasesDraw::One) {
		if (FlagSet(phase, DrawPhase::back)) {
			DrawBackground(surface, model, vsDraw, ll, rcLine, lineRange, posLineStart, xStart,
				subLine, background);
			DrawFoldDisplayText(surface, model, vsDraw, ll, line, xStart, rcLine, subLine, subLineStart, DrawPhase::back);
			DrawEOLAnnotationText(surface, model, vsDraw, ll, line, xStart, rcLine, subLine, subLineStart, DrawPhase::back);
			// Remove drawBack to not draw again in DrawFoldDisplayText
			phase = static_cast<DrawPhase>(static_cast<int>(phase) & ~static_cast<int>(DrawPhase::back));
			DrawEOL(surface, model, vsDraw, ll, rcLine, line, lineRange.end,
				xStart, subLine, subLineStart, background);
			if (vsDraw.IsLineFrameOpaque(model.caret.active, ll->containsCaret)) {
				DrawCaretLineFramed(surface, vsDraw, ll, rcLine, subLine);
			}
		}

		if (FlagSet(phase, DrawPhase::indicatorsBack)) {
			DrawIndicators(surface, model, vsDraw, ll, line, xStart, rcLine, subLine,
				lineRangeIncludingEnd.end, true, tabWidthMinimumPixels);
			DrawEdgeLine(surface, vsDraw, ll, rcLine, lineRange, xStart);
			DrawMarkUnderline(surface, model, vsDraw, line, rcLine);
		}
	}

	if (FlagSet(phase, DrawPhase::text)) {
		if (vsDraw.selection.visible) {
			DrawTranslucentSelection(surface, model, vsDraw, ll, line, rcLine, subLine, lineRange, xStart, tabWidthMinimumPixels, Layer::UnderText);
		}
		DrawTranslucentLineState(surface, model, vsDraw, ll, line, rcLine, subLine, Layer::UnderText);
		DrawForeground(surface, model, vsDraw, ll, lineVisible, rcLine, lineRange, posLineStart, xStart,
			subLine, background);
	}

	if (FlagSet(phase, DrawPhase::indentationGuides)) {
		DrawIndentGuidesOverEmpty(surface, model, vsDraw, ll, line, lineVisible, rcLine, xStart, subLine);
	}

	if (FlagSet(phase, DrawPhase::indicatorsFore)) {
		DrawIndicators(surface, model, vsDraw, ll, line, xStart, rcLine, subLine,
			lineRangeIncludingEnd.end, false, tabWidthMinimumPixels);
	}

	DrawFoldDisplayText(surface, model, vsDraw, ll, line, xStart, rcLine, subLine, subLineStart, phase);
	DrawEOLAnnotationText(surface, model, vsDraw, ll, line, xStart, rcLine, subLine, subLineStart, phase);

	if (phasesDraw == PhasesDraw::One) {
		DrawEOL(surface, model, vsDraw, ll, rcLine, line, lineRange.end,
			xStart, subLine, subLineStart, background);
		if (vsDraw.IsLineFrameOpaque(model.caret.active, ll->containsCaret)) {
			DrawCaretLineFramed(surface, vsDraw, ll, rcLine, subLine);
		}
		DrawEdgeLine(surface, vsDraw, ll, rcLine, lineRange, xStart);
		DrawMarkUnderline(surface, model, vsDraw, line, rcLine);
	}

	if (vsDraw.selection.visible && FlagSet(phase, DrawPhase::selectionTranslucent)) {
		DrawTranslucentSelection(surface, model, vsDraw, ll, line, rcLine, subLine, lineRange, xStart, tabWidthMinimumPixels, Layer::OverText);
	}

	if (FlagSet(phase, DrawPhase::lineTranslucent)) {
		DrawTranslucentLineState(surface, model, vsDraw, ll, line, rcLine, subLine, Layer::OverText);
	}
}

#if 0
static void DrawFoldLines(Surface *surface, const EditModel &model, const ViewStyle &vsDraw, const LineLayout *ll,
	Sci::Line line, PRectangle rcLine, int subLine) {
	const bool lastSubLine = subLine == (ll->lines - 1);
	const bool expanded = model.pcs->GetExpanded(line);
	const FoldLevel level = model.pdoc->GetFoldLevel(line);
	const FoldLevel levelNext = model.pdoc->GetFoldLevel(line + 1);
	if (LevelIsHeader(level) &&
		(LevelNumber(level) < LevelNumber(levelNext))) {
		const ColourRGBA foldLineColour = vsDraw.ElementColour(Element::FoldLine).value_or(
			vsDraw.markers[static_cast<int>(MarkerOutline::Folder)].fore);
		// Paint the line above the fold
		// Paint the line above the fold
		if ((subLine == 0) &&
			((expanded && (FlagSet(model.foldFlags, FoldFlag::LineBeforeExpanded)))
			||
			(!expanded && (FlagSet(model.foldFlags, FoldFlag::LineBeforeContracted))))) {
			surface->FillRectangleAligned(Side(rcLine, Edge::top, 1.0), foldLineColour);
		}
		// Paint the line below the fold
		if (lastSubLine &&
			((expanded && (FlagSet(model.foldFlags, FoldFlag::LineAfterExpanded)))
			||
			(!expanded && (FlagSet(model.foldFlags, FoldFlag::LineAfterContracted))))) {
			surface->FillRectangleAligned(Side(rcLine, Edge::bottom, 1.0), foldLineColour);
			// If contracted fold line drawn then don't overwrite with hidden line
			// as fold lines are more specific then hidden lines.
			if (!expanded) {
				return;
			}
		}
	}
	if (lastSubLine && model.pcs->GetVisible(line) && !model.pcs->GetVisible(line + 1)) {
		std::optional<ColourRGBA> hiddenLineColour = vsDraw.ElementColour(Element::HiddenLine);
		if (hiddenLineColour) {
			surface->FillRectangleAligned(Side(rcLine, Edge::bottom, 1.0), *hiddenLineColour);
		}
	}
}
#endif

void EditView::PaintText(Surface *surfaceWindow, const EditModel &model, PRectangle rcArea,
	PRectangle rcClient, const ViewStyle &vsDraw) {
	// Allow text at start of line to overlap 1 pixel into the margin as this displays
	// serifs and italic stems for aliased text.
	const int leftTextOverlap = ((model.xOffset == 0) && (vsDraw.leftMarginWidth > 0)) ? 1 : 0;

	// Do the painting
	if (rcArea.right > vsDraw.textStart - leftTextOverlap) {

		Surface *surface = surfaceWindow;
		if (bufferedDraw) {
			surface = pixmapLine.get();
			PLATFORM_ASSERT(pixmapLine->Initialised());
			surface->SetMode(model.CurrentSurfaceMode());
		}

		const Point ptOrigin = model.GetVisibleOriginInMain();

		const int screenLinePaintFirst = static_cast<int>(rcArea.top) / vsDraw.lineHeight;
		const int xStart = vsDraw.textStart - model.xOffset + static_cast<int>(ptOrigin.x);

		const SelectionPosition posCaret = model.posDrag.IsValid() ? model.posDrag : model.sel.RangeMain().caret;
		const Sci::Line lineCaret = model.pdoc->SciLineFromPosition(posCaret.Position());
		const int caretOffset = static_cast<int>(posCaret.Position() - model.pdoc->LineStart(lineCaret));

		PRectangle rcTextArea = rcClient;
		if (vsDraw.marginInside) {
			rcTextArea.left += vsDraw.textStart;
			rcTextArea.right -= vsDraw.rightMarginWidth;
		} else {
			rcTextArea = rcArea;
		}

		// Remove selection margin from drawing area so text will not be drawn
		// on it in unbuffered mode.
		const bool clipping = !bufferedDraw && vsDraw.marginInside;
		if (clipping) {
			PRectangle rcClipText = rcTextArea;
			rcClipText.left -= leftTextOverlap;
			surfaceWindow->SetClip(rcClipText);
		}

		// Loop on visible lines
#if defined(TIME_PAINTING)
		double durLayout = 0.0;
		double durPaint = 0.0;
		double durCopy = 0.0;
		const ElapsedPeriod epWhole;
#endif
		const bool bracesIgnoreStyle = ((vsDraw.braceHighlightIndicatorSet && (model.bracesMatchStyle == StyleBraceLight)) ||
			(vsDraw.braceBadLightIndicatorSet && (model.bracesMatchStyle == StyleBraceBad)));

		Sci::Line lineDocPrevious = -1;	// Used to avoid laying out one document line multiple times
		LineLayout *ll = nullptr;
		int phaseCount;
		DrawPhase phases[8];
		if ((phasesDraw == PhasesDraw::Multiple) && !bufferedDraw) {
			phases[0] = DrawPhase::back;
			phases[1] = DrawPhase::indicatorsBack;
			phases[2] = DrawPhase::text;
			phases[3] = DrawPhase::indentationGuides;
			phases[4] = DrawPhase::indicatorsFore;
			phases[5] = DrawPhase::selectionTranslucent;
			phases[6] = DrawPhase::lineTranslucent;
			phases[7] = DrawPhase::carets;
			phaseCount = 8;
		} else {
			phases[0] = DrawPhase::all;
			phaseCount = 1;
		}

		for (int phaseIndex = 0; phaseIndex < phaseCount; phaseIndex++) {
			const DrawPhase phase = phases[phaseIndex];
			int ypos = 0;
			if (!bufferedDraw)
				ypos += screenLinePaintFirst * vsDraw.lineHeight;
			int yposScreen = screenLinePaintFirst * vsDraw.lineHeight;
			Sci::Line visibleLine = model.TopLineOfMain() + screenLinePaintFirst;
			while (visibleLine < model.pcs->LinesDisplayed() && yposScreen < rcArea.bottom) {

				const Sci::Line lineDoc = model.pcs->DocFromDisplay(visibleLine);
				// Only visible lines should be handled by the code within the loop
				PLATFORM_ASSERT(model.pcs->GetVisible(lineDoc));
				const Sci::Line lineStartSet = model.pcs->DisplayFromDoc(lineDoc);
				const int subLine = static_cast<int>(visibleLine - lineStartSet);

				// Copy this line and its styles from the document into local arrays
				// and determine the x position at which each character starts.
#if defined(TIME_PAINTING)
				ElapsedPeriod ep;
#endif
				if (lineDoc != lineDocPrevious) {
					lineDocPrevious = lineDoc;
					ll = RetrieveLineLayout(lineDoc, model);
					LayoutLine(model, surface, vsDraw, ll, model.wrapWidth, LayoutLineOption::KeepPosition);
				}
#if defined(TIME_PAINTING)
				durLayout += ep.Reset();
#endif
				if (ll) {
					ll->containsCaret = vsDraw.selection.visible && (lineDoc == lineCaret)
						&& (ll->lines == 1 || !vsDraw.caretLine.subLine || ll->InLine(caretOffset, subLine));

					PRectangle rcLine = rcTextArea;
					rcLine.top = static_cast<XYPOSITION>(ypos);
					rcLine.bottom = static_cast<XYPOSITION>(ypos + vsDraw.lineHeight);

					const Range rangeLine(model.pdoc->LineStart(lineDoc),
						model.pdoc->LineStart(lineDoc + 1));

					// Highlight the current braces if any
					ll->SetBracesHighlight(rangeLine, model.braces, static_cast<unsigned char>(model.bracesMatchStyle),
						static_cast<int>(model.highlightGuideColumn * vsDraw.aveCharWidth), bracesIgnoreStyle);

					if (leftTextOverlap && (bufferedDraw || ((phasesDraw < PhasesDraw::Multiple) && (FlagSet(phase, DrawPhase::back))))) {
						// Clear the left margin
						PRectangle rcSpacer = rcLine;
						rcSpacer.right = rcSpacer.left;
						rcSpacer.left -= 1;
						surface->FillRectangleAligned(rcSpacer, Fill(vsDraw.styles[StyleDefault].back));
					}

					if (model.BidirectionalEnabled()) {
						// Fill the line bidi data
						UpdateBidiData(model, vsDraw, ll);
					}

					DrawLine(surface, model, vsDraw, ll, lineDoc, visibleLine, xStart, rcLine, subLine, phase);
#if defined(TIME_PAINTING)
					durPaint += ep.Reset();
#endif
					// Restore the previous styles for the brace highlights in case layout is in cache.
					ll->RestoreBracesHighlight(rangeLine, model.braces, bracesIgnoreStyle);
#if 0
					if (FlagSet(phase, DrawPhase::foldLines)) {
						DrawFoldLines(surface, model, vsDraw, ll, lineDoc, rcLine, subLine);
					}
#endif
					if (FlagSet(phase, DrawPhase::carets)) {
						DrawCarets(surface, model, vsDraw, ll, lineDoc, xStart, rcLine, subLine);
					}

					if (bufferedDraw) {
						const Point from = Point::FromInts(vsDraw.textStart - leftTextOverlap, 0);
						const PRectangle rcCopyArea = PRectangle::FromInts(vsDraw.textStart - leftTextOverlap, yposScreen,
							static_cast<int>(rcClient.right - vsDraw.rightMarginWidth),
							yposScreen + vsDraw.lineHeight);
						pixmapLine->FlushDrawing();
						surfaceWindow->Copy(rcCopyArea, from, *pixmapLine);
					}

					lineWidthMaxSeen = std::max(
						lineWidthMaxSeen, static_cast<int>(ll->positions[ll->numCharsInLine]));
#if defined(TIME_PAINTING)
					durCopy += ep.Duration();
#endif
				}

				if (!bufferedDraw) {
					ypos += vsDraw.lineHeight;
				}

				yposScreen += vsDraw.lineHeight;
				visibleLine++;
			}
		}
#if defined(TIME_PAINTING)
		if (durPaint < 0.00000001)
			durPaint = 0.00000001;
#endif
		// Right column limit indicator
		PRectangle rcBeyondEOF = (vsDraw.marginInside) ? rcClient : rcArea;
		rcBeyondEOF.left = static_cast<XYPOSITION>(vsDraw.textStart);
		rcBeyondEOF.right = rcBeyondEOF.right - ((vsDraw.marginInside) ? vsDraw.rightMarginWidth : 0);
		rcBeyondEOF.top = static_cast<XYPOSITION>((model.pcs->LinesDisplayed() - model.TopLineOfMain()) * vsDraw.lineHeight);
		if (rcBeyondEOF.top < rcBeyondEOF.bottom) {
			surfaceWindow->FillRectangleAligned(rcBeyondEOF, Fill(vsDraw.styles[StyleDefault].back));
			if (vsDraw.edgeState == EdgeVisualStyle::Line) {
				const int edgeX = static_cast<int>(vsDraw.theEdge.column * vsDraw.aveCharWidth);
				rcBeyondEOF.left = static_cast<XYPOSITION>(edgeX + xStart);
				rcBeyondEOF.right = rcBeyondEOF.left + 1;
				surfaceWindow->FillRectangleAligned(rcBeyondEOF, Fill(vsDraw.theEdge.colour));
			} else if (vsDraw.edgeState == EdgeVisualStyle::MultiLine) {
				for (size_t edge = 0; edge < vsDraw.theMultiEdge.size(); edge++) {
					if (vsDraw.theMultiEdge[edge].column >= 0) {
						const int edgeX = static_cast<int>(vsDraw.theMultiEdge[edge].column * vsDraw.aveCharWidth);
						rcBeyondEOF.left = static_cast<XYPOSITION>(edgeX + xStart);
						rcBeyondEOF.right = rcBeyondEOF.left + 1;
						surfaceWindow->FillRectangleAligned(rcBeyondEOF, Fill(vsDraw.theMultiEdge[edge].colour));
					}
				}
			}
		}

		if (clipping)
			surfaceWindow->PopClip();

		//Platform::DebugPrintf("start display %d, offset = %d\n", model.pdoc->LengthNoExcept(), model.xOffset);
#if defined(TIME_PAINTING)
		Platform::DebugPrintf(
			"Layout:%9.6g    Paint:%9.6g    Ratio:%9.6g   Copy:%9.6g   Total:%9.6g\n",
			durLayout, durPaint, durLayout / durPaint, durCopy, epWhole.Duration());
#endif
	}
}

void EditView::FillLineRemainder(Surface *surface, const EditModel &model, const ViewStyle &vsDraw, const LineLayout *ll,
	Sci::Line line, PRectangle rcArea, int subLine) {
	InSelection eolInSelection = InSelection::inNone;
	if (vsDraw.selection.visible && (subLine == (ll->lines - 1))) {
		eolInSelection = model.LineEndInSelection(line);
	}

	const std::optional<ColourRGBA> background = vsDraw.Background(model.GetMark(line), model.caret.active, ll->containsCaret);
	const bool drawEOLSelection = eolInSelection && vsDraw.selection.eolFilled && (line < model.pdoc->LinesTotal() - 1);

	if (drawEOLSelection && (vsDraw.selection.layer == Layer::Base)) {
		surface->FillRectangleAligned(rcArea, Fill(SelectionBackground(model, vsDraw, eolInSelection).Opaque()));
	} else {
		if (background) {
			surface->FillRectangleAligned(rcArea, Fill(*background));
		} else if (vsDraw.styles[ll->styles[ll->numCharsInLine]].eolFilled) {
			surface->FillRectangleAligned(rcArea, Fill(vsDraw.styles[ll->styles[ll->numCharsInLine]].back));
		} else {
			surface->FillRectangleAligned(rcArea, Fill(vsDraw.styles[StyleDefault].back));
		}
		if (drawEOLSelection && (vsDraw.selection.layer != Layer::Base)) {
			surface->FillRectangleAligned(rcArea, SelectionBackground(model, vsDraw, eolInSelection));
		}
	}
}

// Space (3 space characters) between line numbers and text when printing.
#define lineNumberPrintSpace "   "

static ColourRGBA InvertedLight(ColourRGBA orig) noexcept {
	unsigned int r = orig.GetRed();
	unsigned int g = orig.GetGreen();
	unsigned int b = orig.GetBlue();
	const unsigned int l = (r + g + b) / 3; 	// There is a better calculation for this that matches human eye
	const unsigned int il = 0xff - l;
	if (l == 0)
		return ColourRGBA(0xff, 0xff, 0xff);
	r = r * il / l;
	g = g * il / l;
	b = b * il / l;
	return ColourRGBA(std::min(r, 0xffu), std::min(g, 0xffu), std::min(b, 0xffu));
}

Sci::Position EditView::FormatRange(bool draw, CharacterRangeFull chrg, Scintilla::Rectangle rc, Surface *surface, Surface *surfaceMeasure,
	const EditModel &model, const ViewStyle &vs) {
	// Can't use measurements cached for screen
	posCache.Clear();

	ViewStyle vsPrint(vs);
	vsPrint.technology = Technology::Default;

	// Modify the view style for printing as do not normally want any of the transient features to be printed
	// Printing supports only the line number margin.
	int lineNumberIndex = -1;
	int margin = 0;
	for (auto &style : vsPrint.ms) {
		if (style.style == MarginType::Number && style.width > 0) {
			lineNumberIndex = margin;
		} else {
			style.width = 0;
		}
		++margin;
	}
	vsPrint.fixedColumnWidth = 0;
	vsPrint.zoomLevel = printParameters.magnification;
	// Don't show indentation guides
	// If this ever gets changed, cached pixmap would need to be recreated if technology != Technology::Default
	vsPrint.viewIndentationGuides = IndentView::None;
	// Don't show the selection when printing
	vsPrint.selection.visible = false;
	vsPrint.elementColours.clear();
	vsPrint.elementBaseColours.clear();
	vsPrint.caretLine.alwaysShow = false;
	// Don't highlight matching braces using indicators
	vsPrint.braceHighlightIndicatorSet = false;
	vsPrint.braceBadLightIndicatorSet = false;

	// Set colours for printing according to users settings
	const PrintOption colourMode = printParameters.colourMode;
	const auto endStyles = (colourMode == PrintOption::ColourOnWhiteDefaultBG) ?
		vsPrint.styles.begin() + StyleLineNumber : vsPrint.styles.end();
	for (auto it = vsPrint.styles.begin(); it < endStyles; ++it) {
		if (colourMode == PrintOption::InvertLight) {
			it->fore = InvertedLight(it->fore);
			it->back = InvertedLight(it->back);
		} else if (colourMode == PrintOption::BlackOnWhite) {
			it->fore = ColourRGBA(0, 0, 0);
			it->back = ColourRGBA(0xff, 0xff, 0xff);
		} else if (colourMode == PrintOption::ColourOnWhite || colourMode == PrintOption::ColourOnWhiteDefaultBG) {
			it->back = ColourRGBA(0xff, 0xff, 0xff);
		}
	}
	// White background for the line numbers if PrintOption::ScreenColours isn't used
	if (colourMode != PrintOption::ScreenColours) {
		vsPrint.styles[StyleLineNumber].back = ColourRGBA(0xff, 0xff, 0xff);
	}

	// Printing uses different margins, so reset screen margins
	vsPrint.leftMarginWidth = 0;
	vsPrint.rightMarginWidth = 0;

	vsPrint.Refresh(*surfaceMeasure, model.pdoc->tabInChars);
	// Determining width must happen after fonts have been realised in Refresh
	int lineNumberWidth = 0;
	if (lineNumberIndex >= 0) {
		lineNumberWidth = static_cast<int>(std::lround(surfaceMeasure->WidthText(vsPrint.styles[StyleLineNumber].font.get(),
			"99999" lineNumberPrintSpace)));
		vsPrint.ms[lineNumberIndex].width = lineNumberWidth;
		vsPrint.Refresh(*surfaceMeasure, model.pdoc->tabInChars);	// Recalculate fixedColumnWidth
	}

	const Sci::Line linePrintStart = model.pdoc->SciLineFromPosition(chrg.cpMin);
	const Sci::Line linePrintMax = model.pdoc->SciLineFromPosition(chrg.cpMax);
	Sci::Line linePrintLast = linePrintStart + (rc.bottom - rc.top) / vsPrint.lineHeight - 1;
	linePrintLast = std::clamp(linePrintLast, linePrintStart, linePrintMax);
	//Platform::DebugPrintf("Formatting lines=[%0d,%0d,%0d] top=%0d bottom=%0d line=%0d %.0f\n",
	//      linePrintStart, linePrintLast, linePrintMax, rc.top, rc.bottom, vsPrint.lineHeight,
	//      surfaceMeasure->Height(vsPrint.styles[StyleLineNumber].font));
	Sci::Position endPosPrint = model.pdoc->LengthNoExcept();
	if (linePrintLast < model.pdoc->LinesTotal())
		endPosPrint = model.pdoc->LineStart(linePrintLast + 1);

	// Ensure we are styled to where we are formatting.
	model.pdoc->EnsureStyledTo(endPosPrint);

	const int xStart = vsPrint.fixedColumnWidth + rc.left;
	int ypos = rc.top;

	Sci::Line lineDoc = linePrintStart;

	Sci::Position nPrintPos = chrg.cpMin;
	int visibleLine = 0;
	const int widthPrint = (printParameters.wrapState == Wrap::None) ? LineLayout::wrapWidthInfinite : rc.right - rc.left - vsPrint.fixedColumnWidth;

	while (lineDoc <= linePrintLast && ypos < rc.bottom) {

		// When printing, the hdc and hdcTarget may be the same, so
		// changing the state of surfaceMeasure may change the underlying
		// state of surface. Therefore, any cached state is discarded before
		// using each surface.
		surfaceMeasure->FlushCachedState();

		// Copy this line and its styles from the document into local arrays
		// and determine the x position at which each character starts.
		LineLayout ll(lineDoc, static_cast<int>(model.pdoc->LineStart(lineDoc + 1) - model.pdoc->LineStart(lineDoc) + 1));
		LayoutLine(model, surfaceMeasure, vsPrint, &ll, widthPrint, LayoutLineOption::Printing, ll.maxLineLength);

		ll.containsCaret = false;

		PRectangle rcLine = PRectangle::FromInts(
			rc.left,
			ypos,
			rc.right - 1,
			ypos + vsPrint.lineHeight);

		// When document line is wrapped over multiple display lines, find where
		// to start printing from to ensure a particular position is on the first
		// line of the page.
		if (visibleLine == 0) {
			const Sci::Position startWithinLine = nPrintPos -
				model.pdoc->LineStart(lineDoc);
			for (int iwl = 0; iwl < ll.lines - 1; iwl++) {
				if (ll.LineStart(iwl) <= startWithinLine && ll.LineStart(iwl + 1) >= startWithinLine) {
					visibleLine = -iwl;
				}
			}

			if (ll.lines > 1 && startWithinLine >= ll.LineStart(ll.lines - 1)) {
				visibleLine = -(ll.lines - 1);
			}
		}

		if (draw && lineNumberWidth &&
			(ypos + vsPrint.lineHeight <= rc.bottom) &&
			(visibleLine >= 0)) {
			const std::string number = std::to_string(lineDoc + 1) + lineNumberPrintSpace;
			PRectangle rcNumber = rcLine;
			rcNumber.right = rcNumber.left + lineNumberWidth;
			// Right justify
			rcNumber.left = rcNumber.right - surfaceMeasure->WidthText(
				vsPrint.styles[StyleLineNumber].font.get(), number);
			surface->FlushCachedState();
			surface->DrawTextNoClip(rcNumber, vsPrint.styles[StyleLineNumber].font.get(),
				ypos + vsPrint.maxAscent, number,
				vsPrint.styles[StyleLineNumber].fore,
				vsPrint.styles[StyleLineNumber].back);
		}

		// Draw the line
		surface->FlushCachedState();

		for (int iwl = 0; iwl < ll.lines; iwl++) {
			if (ypos + vsPrint.lineHeight <= rc.bottom) {
				if (visibleLine >= 0) {
					if (draw) {
						rcLine.top = static_cast<XYPOSITION>(ypos);
						rcLine.bottom = static_cast<XYPOSITION>(ypos + vsPrint.lineHeight);
						DrawLine(surface, model, vsPrint, &ll, lineDoc, visibleLine, xStart, rcLine, iwl, DrawPhase::all);
					}
					ypos += vsPrint.lineHeight;
				}
				visibleLine++;
				if (iwl == ll.lines - 1)
					nPrintPos = model.pdoc->LineStart(lineDoc + 1);
				else
					nPrintPos += ll.LineStart(iwl + 1) - ll.LineStart(iwl);
			}
		}

		++lineDoc;
	}

	// Clear cache so measurements are not used for screen
	posCache.Clear();

	return nPrintPos;
}
