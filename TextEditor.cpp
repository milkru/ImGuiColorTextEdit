#include <algorithm>
#include <chrono>
#include <string>
#include <regex>
#include <cmath>

#include "TextEditor.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h" // for imGui::GetCurrentWindow()

// TODO
// - multiline comments vs single-line: latter is blocking start of a ML

template<class InputIt1, class InputIt2, class BinaryPredicate>
bool equals(InputIt1 first1, InputIt1 last1,
	InputIt2 first2, InputIt2 last2, BinaryPredicate p)
{
	for (; first1 != last1 && first2 != last2; ++first1, ++first2)
	{
		if (!p(*first1, *first2))
		{
			return false;
		}
	}

	return first1 == last1 && first2 == last2;
}

TextEditor::TextEditor()
	: mLineSpacing(1.0f)
	, mUndoIndex(0)
	, mTabSize(4)
	, mOverwrite(false)
	, mReadOnly(false)
	, mWithinRender(false)
	, mScrollToCursor(false)
	, mScrollToTop(false)
	, mTextChanged(false)
	, mColorizerEnabled(true)
	, mTextStart(20.0f)
	, mLeftMargin(10)
	, mCursorPositionChanged(false)
	, mColorRangeMin(0)
	, mColorRangeMax(0)
	, mSelectionMode(SelectionMode::Normal)
	, mCheckComments(true)
	, mLastClick(-1.0f)
	, mHandleKeyboardInputs(true)
	, mHandleMouseInputs(true)
	, mIgnoreImGuiChild(false)
	, mShowWhitespaces(true)
	, mStartTime(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count())
{
	SetPalette(GetColorPalette());
	SetLanguageDefinition(LanguageDefinition::HLSL());
	mLines.push_back(Line());
}

TextEditor::~TextEditor()
{}

void TextEditor::SetLanguageDefinition(const LanguageDefinition & aLanguageDef)
{
	mLanguageDefinition = aLanguageDef;
	mRegexList.clear();

	for (auto& r : mLanguageDefinition.mTokenRegexStrings)
	{
		mRegexList.push_back(std::make_pair(std::regex(r.first, std::regex_constants::optimize), r.second));
	}

	Colorize();
}

void TextEditor::SetPalette(const Palette & aValue)
{
	mPaletteBase = aValue;
}

std::string TextEditor::GetText(const Coordinates & aStart, const Coordinates & aEnd) const
{
	std::string result;

	auto lstart = aStart.mLine;
	auto lend = aEnd.mLine;
	auto istart = GetCharacterIndex(aStart);
	auto iend = GetCharacterIndex(aEnd);
	size_t s = 0;

	for (size_t i = lstart; i < lend; i++)
	{
		s += mLines[i].size();
	}

	result.reserve(s + s / 8);

	while (istart < iend || lstart < lend)
	{
		if (lstart >= (int)mLines.size())
		{
			break;
		}

		auto& line = mLines[lstart];
		if (istart < (int)line.size())
		{
			result += line[istart].mChar;
			istart++;
		}
		else
		{
			istart = 0;
			++lstart;
			result += '\n';
		}
	}

	return result;
}

TextEditor::Coordinates TextEditor::GetActualCursorCoordinates() const
{
	return SanitizeCoordinates(mState.mCursorPosition);
}

TextEditor::Coordinates TextEditor::SanitizeCoordinates(const Coordinates & aValue) const
{
	auto line = aValue.mLine;
	auto column = aValue.mColumn;
	if (line >= (int)mLines.size())
	{
		if (mLines.empty())
		{
			line = 0;
			column = 0;
		}
		else
		{
			line = (int)mLines.size() - 1;
			column = GetLineMaxColumn(line);
		}

		return Coordinates(line, column);
	}
	else
	{
		column = mLines.empty() ? 0 : std::min(column, GetLineMaxColumn(line));
		return Coordinates(line, column);
	}
}

// https://en.wikipedia.org/wiki/UTF-8
// We assume that the char is a standalone character (<128) or a leading byte of an UTF-8 code sequence (non-10xxxxxx code)
static int UTF8CharLength(TextEditor::Char c)
{
	if ((c & 0xFE) == 0xFC)
	{
		return 6;
	}

	if ((c & 0xFC) == 0xF8)
	{
		return 5;
	}

	if ((c & 0xF8) == 0xF0)
	{
		return 4;
	}
	else if ((c & 0xF0) == 0xE0)
	{
		return 3;
	}
	else if ((c & 0xE0) == 0xC0)
	{
		return 2;
	}

	return 1;
}

static inline int ImTextCharToUtf8(char* buf, int buf_size, unsigned int c)
{
	if (c < 0x80)
	{
		buf[0] = (char)c;
		return 1;
	}

	if (c < 0x800)
	{
		if (buf_size < 2) return 0;
		buf[0] = (char)(0xc0 + (c >> 6));
		buf[1] = (char)(0x80 + (c & 0x3f));
		return 2;
	}

	if (c >= 0xdc00 && c < 0xe000)
	{
		return 0;
	}

	if (c >= 0xd800 && c < 0xdc00)
	{
		if (buf_size < 4) return 0;
		buf[0] = (char)(0xf0 + (c >> 18));
		buf[1] = (char)(0x80 + ((c >> 12) & 0x3f));
		buf[2] = (char)(0x80 + ((c >> 6) & 0x3f));
		buf[3] = (char)(0x80 + ((c) & 0x3f));
		return 4;
	}
	
	if (buf_size < 3)
	{
		return 0;
	}

	buf[0] = (char)(0xe0 + (c >> 12));
	buf[1] = (char)(0x80 + ((c >> 6) & 0x3f));
	buf[2] = (char)(0x80 + ((c) & 0x3f));

	return 3;
}

void TextEditor::Advance(Coordinates & aCoordinates) const
{
	if (aCoordinates.mLine < (int)mLines.size())
	{
		auto& line = mLines[aCoordinates.mLine];
		auto cindex = GetCharacterIndex(aCoordinates);

		if (cindex + 1 < (int)line.size())
		{
			auto delta = UTF8CharLength(line[cindex].mChar);
			cindex = std::min(cindex + delta, (int)line.size() - 1);
		}
		else
		{
			++aCoordinates.mLine;
			cindex = 0;
		}

		aCoordinates.mColumn = GetCharacterColumn(aCoordinates.mLine, cindex);
	}
}

void TextEditor::DeleteRange(const Coordinates & aStart, const Coordinates & aEnd)
{
	assert(aEnd >= aStart);
	assert(!mReadOnly);

	if (aEnd == aStart)
		return;

	auto start = GetCharacterIndex(aStart);
	auto end = GetCharacterIndex(aEnd);

	if (aStart.mLine == aEnd.mLine)
	{
		auto& line = mLines[aStart.mLine];
		auto n = GetLineMaxColumn(aStart.mLine);

		if (aEnd.mColumn >= n)
		{
			line.erase(line.begin() + start, line.end());
		}
		else
		{
			line.erase(line.begin() + start, line.begin() + end);
		}
	}
	else
	{
		auto& firstLine = mLines[aStart.mLine];
		auto& lastLine = mLines[aEnd.mLine];

		firstLine.erase(firstLine.begin() + start, firstLine.end());
		lastLine.erase(lastLine.begin(), lastLine.begin() + end);

		if (aStart.mLine < aEnd.mLine)
		{
			firstLine.insert(firstLine.end(), lastLine.begin(), lastLine.end());
		}

		if (aStart.mLine < aEnd.mLine)
		{
			RemoveLine(aStart.mLine + 1, aEnd.mLine + 1);
		}
	}

	mTextChanged = true;
}

int TextEditor::InsertTextAt(Coordinates& /* inout */ aWhere, const char * aValue)
{
	assert(!mReadOnly);

	int cindex = GetCharacterIndex(aWhere);
	int totalLines = 0;
	while (*aValue != '\0')
	{
		assert(!mLines.empty());

		if (*aValue == '\r')
		{
			++aValue;
		}
		else if (*aValue == '\n')
		{
			if (cindex < (int)mLines[aWhere.mLine].size())
			{
				auto& newLine = InsertLine(aWhere.mLine + 1);
				auto& line = mLines[aWhere.mLine];
				newLine.insert(newLine.begin(), line.begin() + cindex, line.end());
				line.erase(line.begin() + cindex, line.end());
			}
			else
			{
				InsertLine(aWhere.mLine + 1);
			}

			++aWhere.mLine;
			aWhere.mColumn = 0;
			cindex = 0;
			++totalLines;
			++aValue;
		}
		else
		{
			auto& line = mLines[aWhere.mLine];
			auto d = UTF8CharLength(*aValue);
			while (d-- > 0 && *aValue != '\0')
			{
				line.insert(line.begin() + cindex++, Glyph(*aValue++, PaletteIndex::Default));
			}

			aWhere.mColumn = GetCharacterColumn(aWhere.mLine, cindex);
		}

		mTextChanged = true;
	}

	return totalLines;
}

void TextEditor::AddUndo(UndoRecord& aValue)
{
	assert(!mReadOnly);

	mUndoBuffer.resize((size_t)(mUndoIndex + 1));
	mUndoBuffer.back() = aValue;
	++mUndoIndex;
}

TextEditor::Coordinates TextEditor::ScreenPosToCoordinates(const ImVec2& aPosition) const
{
	ImVec2 origin = ImGui::GetCursorScreenPos();
	ImVec2 local(aPosition.x - origin.x, aPosition.y - origin.y);

	int lineNo = std::max(0, (int)floor(local.y / mCharAdvance.y));

	int columnCoord = 0;

	if (lineNo >= 0 && lineNo < (int)mLines.size())
	{
		auto& line = mLines.at(lineNo);

		int columnIndex = 0;
		float columnX = 0.0f;

		while ((size_t)columnIndex < line.size())
		{
			float columnWidth = 0.0f;

			if (line[columnIndex].mChar == '\t')
			{
				float spaceSize = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, " ").x;
				float oldX = columnX;
				float newColumnX = (1.0f + std::floor((1.0f + columnX) / (float(mTabSize) * spaceSize))) * (float(mTabSize) * spaceSize);
				columnWidth = newColumnX - oldX;
				if (mTextStart + columnX + columnWidth * 0.5f > local.x)
				{
					break;
				}

				columnX = newColumnX;
				columnCoord = (columnCoord / mTabSize) * mTabSize + mTabSize;
				columnIndex++;
			}
			else
			{
				char buf[7];
				auto d = UTF8CharLength(line[columnIndex].mChar);
				int i = 0;
				
				while (i < 6 && d-- > 0)
				{
					buf[i++] = line[columnIndex++].mChar;
				}
				
				buf[i] = '\0';
				columnWidth = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, buf).x;
				
				if (mTextStart + columnX + columnWidth * 0.5f > local.x)
				{
					break;
				}

				columnX += columnWidth;
				columnCoord++;
			}
		}
	}

	return SanitizeCoordinates(Coordinates(lineNo, columnCoord));
}

TextEditor::Coordinates TextEditor::FindWordStart(const Coordinates & aFrom) const
{
	Coordinates at = aFrom;
	if (at.mLine >= (int)mLines.size())
	{
		return at;
	}

	auto& line = mLines[at.mLine];
	auto cindex = GetCharacterIndex(at);

	if (cindex >= (int)line.size())
	{
		return at;
	}

	while (cindex > 0 && isspace(line[cindex].mChar))
	{
		--cindex;
	}

	auto cstart = (PaletteIndex)line[cindex].mColorIndex;
	while (cindex > 0)
	{
		auto c = line[cindex].mChar;
		if ((c & 0xC0) != 0x80)	// not UTF code sequence 10xxxxxx
		{
			if (c <= 32 && isspace(c))
			{
				cindex++;
				break;
			}

			if (cstart != (PaletteIndex)line[size_t(cindex - 1)].mColorIndex)
			{
				break;
			}
		}

		--cindex;
	}

	return Coordinates(at.mLine, GetCharacterColumn(at.mLine, cindex));
}

TextEditor::Coordinates TextEditor::FindWordEnd(const Coordinates & aFrom) const
{
	Coordinates at = aFrom;
	if (at.mLine >= (int)mLines.size())
	{
		return at;
	}

	auto& line = mLines[at.mLine];
	auto cindex = GetCharacterIndex(at);

	if (cindex >= (int)line.size())
	{
		return at;
	}

	bool prevspace = (bool)isspace(line[cindex].mChar);
	auto cstart = (PaletteIndex)line[cindex].mColorIndex;
	while (cindex < (int)line.size())
	{
		auto c = line[cindex].mChar;
		auto d = UTF8CharLength(c);
		if (cstart != (PaletteIndex)line[cindex].mColorIndex)
		{
			break;
		}

		if (prevspace != !!isspace(c))
		{
			if (isspace(c))
			{
				while (cindex < (int)line.size() && isspace(line[cindex].mChar))
				{
					++cindex;
				}
			}

			break;
		}

		cindex += d;
	}

	return Coordinates(aFrom.mLine, GetCharacterColumn(aFrom.mLine, cindex));
}

TextEditor::Coordinates TextEditor::FindNextWord(const Coordinates & aFrom) const
{
	Coordinates at = aFrom;
	if (at.mLine >= (int)mLines.size())
	{
		return at;
	}

	// skip to the next non-word character
	auto cindex = GetCharacterIndex(aFrom);
	bool isword = false;
	bool skip = false;
	if (cindex < (int)mLines[at.mLine].size())
	{
		auto& line = mLines[at.mLine];
		isword = isalnum(line[cindex].mChar);
		skip = isword;
	}

	while (!isword || skip)
	{
		if (at.mLine >= mLines.size())
		{
			auto l = std::max(0, (int) mLines.size() - 1);
			return Coordinates(l, GetLineMaxColumn(l));
		}

		auto& line = mLines[at.mLine];
		if (cindex < (int)line.size())
		{
			isword = isalnum(line[cindex].mChar);

			if (isword && !skip)
			{
				return Coordinates(at.mLine, GetCharacterColumn(at.mLine, cindex));
			}

			if (!isword)
			{
				skip = false;
			}

			cindex++;
		}
		else
		{
			cindex = 0;
			++at.mLine;
			skip = false;
			isword = false;
		}
	}

	return at;
}

int TextEditor::GetCharacterIndex(const Coordinates& aCoordinates) const
{
	if (aCoordinates.mLine >= mLines.size())
	{
		return -1;
	}

	auto& line = mLines[aCoordinates.mLine];
	int c = 0;
	int i = 0;

	for (; i < line.size() && c < aCoordinates.mColumn;)
	{
		if (line[i].mChar == '\t')
		{
			c = (c / mTabSize) * mTabSize + mTabSize;
		}
		else
		{
			++c;
		}

		i += UTF8CharLength(line[i].mChar);
	}

	return i;
}

int TextEditor::GetCharacterColumn(int aLine, int aIndex) const
{
	if (aLine >= mLines.size())
	{
		return 0;
	}

	auto& line = mLines[aLine];
	int col = 0;
	int i = 0;
	while (i < aIndex && i < (int)line.size())
	{
		auto c = line[i].mChar;
		i += UTF8CharLength(c);

		if (c == '\t')
		{
			col = (col / mTabSize) * mTabSize + mTabSize;
		}
		else
		{
			col++;
		}
	}

	return col;
}

int TextEditor::GetLineCharacterCount(int aLine) const
{
	if (aLine >= mLines.size())
	{
		return 0;
	}

	auto& line = mLines[aLine];
	int c = 0;

	for (unsigned i = 0; i < line.size(); c++)
	{
		i += UTF8CharLength(line[i].mChar);
	}

	return c;
}

int TextEditor::GetLineMaxColumn(int aLine) const
{
	if (aLine >= mLines.size())
	{
		return 0;
	}

	auto& line = mLines[aLine];
	int col = 0;
	for (unsigned i = 0; i < line.size(); )
	{
		auto c = line[i].mChar;
		if (c == '\t')
		{
			col = (col / mTabSize) * mTabSize + mTabSize;
		}
		else
		{
			col++;
		}

		i += UTF8CharLength(c);
	}

	return col;
}

bool TextEditor::IsOnWordBoundary(const Coordinates & aAt) const
{
	if (aAt.mLine >= (int)mLines.size() || aAt.mColumn == 0)
	{
		return true;
	}

	auto& line = mLines[aAt.mLine];
	auto cindex = GetCharacterIndex(aAt);
	if (cindex >= (int)line.size())
	{
		return true;
	}

	if (mColorizerEnabled)
	{
		return line[cindex].mColorIndex != line[size_t(cindex - 1)].mColorIndex;
	}

	return isspace(line[cindex].mChar) != isspace(line[cindex - 1].mChar);
}

void TextEditor::RemoveLine(int aStart, int aEnd)
{
	assert(!mReadOnly);
	assert(aEnd >= aStart);
	assert(mLines.size() > (size_t)(aEnd - aStart));

	ErrorMarkers etmp;
	for (auto& i : mErrorMarkers)
	{
		ErrorMarkers::value_type e(i.first >= aStart ? i.first - 1 : i.first, i.second);
		if (e.first >= aStart && e.first <= aEnd)
		{
			continue;
		}

		etmp.insert(e);
	}

	mErrorMarkers = std::move(etmp);

	Breakpoints btmp;
	for (auto i : mBreakpoints)
	{
		if (i >= aStart && i <= aEnd)
		{
			continue;
		}

		btmp.insert(i >= aStart ? i - 1 : i);
	}

	mBreakpoints = std::move(btmp);

	mLines.erase(mLines.begin() + aStart, mLines.begin() + aEnd);
	assert(!mLines.empty());

	mTextChanged = true;
}

void TextEditor::RemoveLine(int aIndex)
{
	assert(!mReadOnly);
	assert(mLines.size() > 1);

	ErrorMarkers etmp;
	for (auto& i : mErrorMarkers)
	{
		ErrorMarkers::value_type e(i.first > aIndex ? i.first - 1 : i.first, i.second);
		if (e.first - 1 == aIndex)
		{
			continue;
		}

		etmp.insert(e);
	}

	mErrorMarkers = std::move(etmp);

	Breakpoints btmp;
	for (auto i : mBreakpoints)
	{
		if (i == aIndex)
		{
			continue;
		}

		btmp.insert(i >= aIndex ? i - 1 : i);
	}

	mBreakpoints = std::move(btmp);

	mLines.erase(mLines.begin() + aIndex);
	assert(!mLines.empty());

	mTextChanged = true;
}

TextEditor::Line& TextEditor::InsertLine(int aIndex)
{
	assert(!mReadOnly);

	auto& result = *mLines.insert(mLines.begin() + aIndex, Line());

	ErrorMarkers etmp;
	for (auto& i : mErrorMarkers)
	{
		etmp.insert(ErrorMarkers::value_type(i.first >= aIndex ? i.first + 1 : i.first, i.second));
	}

	mErrorMarkers = std::move(etmp);

	Breakpoints btmp;
	for (auto i : mBreakpoints)
	{
		btmp.insert(i >= aIndex ? i + 1 : i);
	}

	mBreakpoints = std::move(btmp);

	return result;
}

std::string TextEditor::GetWordUnderCursor() const
{
	auto c = GetCursorPosition();
	return GetWordAt(c);
}

std::string TextEditor::GetWordAt(const Coordinates & aCoords) const
{
	auto start = FindWordStart(aCoords);
	auto end = FindWordEnd(aCoords);

	std::string r;

	auto istart = GetCharacterIndex(start);
	auto iend = GetCharacterIndex(end);

	for (auto it = istart; it < iend; ++it)
	{
		r.push_back(mLines[aCoords.mLine][it].mChar);
	}

	return r;
}

ImU32 TextEditor::GetGlyphColor(const Glyph & aGlyph) const
{
	if (!mColorizerEnabled)
	{
		return mPalette[(int)PaletteIndex::Default];
	}

	if (aGlyph.mComment)
	{
		return mPalette[(int)PaletteIndex::Comment];
	}

	if (aGlyph.mMultiLineComment)
	{
		return mPalette[(int)PaletteIndex::MultiLineComment];
	}

	auto const color = mPalette[(int)aGlyph.mColorIndex];
	if (aGlyph.mPreprocessor)
	{
		const auto ppcolor = mPalette[(int)PaletteIndex::Preprocessor];
		const int c0 = ((ppcolor & 0xff) + (color & 0xff)) / 2;
		const int c1 = (((ppcolor >> 8) & 0xff) + ((color >> 8) & 0xff)) / 2;
		const int c2 = (((ppcolor >> 16) & 0xff) + ((color >> 16) & 0xff)) / 2;
		const int c3 = (((ppcolor >> 24) & 0xff) + ((color >> 24) & 0xff)) / 2;
		return ImU32(c0 | (c1 << 8) | (c2 << 16) | (c3 << 24));
	}

	return color;
}

void TextEditor::HandleKeyboardInputs()
{
	if (ImGui::IsWindowFocused())
	{
		if (ImGui::IsWindowHovered())
			ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
		//ImGui::CaptureKeyboardFromApp(true);

		ImGuiIO& io = ImGui::GetIO();
		auto isOSX = io.ConfigMacOSXBehaviors;
		auto alt = io.KeyAlt;
		auto ctrl = io.KeyCtrl;
		auto shift = io.KeyShift;
		auto super = io.KeySuper;

		auto isShortcut = (isOSX ? (super && !ctrl) : (ctrl && !super)) && !alt && !shift;
		auto isShiftShortcut = (isOSX ? (super && !ctrl) : (ctrl && !super)) && shift && !alt;
		auto isWordmoveKey = isOSX ? alt : ctrl;
		auto isAltOnly = alt && !ctrl && !shift && !super;
		auto isCtrlOnly = ctrl && !alt && !shift && !super;
		auto isShiftOnly = shift && !alt && !ctrl && !super;

		io.WantCaptureKeyboard = true;
		io.WantTextInput = true;

		if (!IsReadOnly() && isShortcut && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Z)))
		{
			Undo();
		}
		else if (!IsReadOnly() && isAltOnly && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Backspace)))
		{
			Undo();
		}
		else if (!IsReadOnly() && isShortcut && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Y)))
		{
			Redo();
		}
		else if (!IsReadOnly() && isShiftShortcut && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Z)))
		{
			Redo();
		}
		else if (!alt && !ctrl && !super && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_UpArrow)))
		{
			MoveUp(1, shift);
		}
		else if (!alt && !ctrl && !super && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_DownArrow)))
		{
			MoveDown(1, shift);
		}
		else if ((isOSX ? !ctrl : !alt) && !super && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_LeftArrow)))
		{
			MoveLeft(1, shift, isWordmoveKey);
		}
		else if ((isOSX ? !ctrl : !alt) && !super && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_RightArrow)))
		{
			MoveRight(1, shift, isWordmoveKey);
		}
		else if (!alt && !ctrl && !super && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_PageUp)))
		{
			MoveUp(GetPageSize() - 4, shift);
		}
		else if (!alt && !ctrl && !super && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_PageDown)))
		{
			MoveDown(GetPageSize() - 4, shift);
		}
		else if (ctrl && !alt && !super && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Home)))
		{
			MoveTop(shift);
		}
		else if (ctrl && !alt && !super && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_End)))
		{
			MoveBottom(shift);
		}
		else if (!alt && !ctrl && !super && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Home)))
		{
			MoveHome(shift);
		}
		else if (!alt && !ctrl && !super && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_End)))
		{
			MoveEnd(shift);
		}
		else if (!IsReadOnly() && !alt && !ctrl && !shift && !super && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Delete)))
		{
			Delete();
		}
		else if (!IsReadOnly() && !alt && !ctrl && !shift && !super && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Backspace)))
		{
			Backspace();
		}
		else if (!alt && !ctrl && !shift && !super && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Insert)))
		{
			mOverwrite ^= true;
		}
		else if (isCtrlOnly && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Insert)))
		{
			Copy();
		}
		else if (isShortcut && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_C)))
		{
			Copy();
		}
		else if (!IsReadOnly() && isShiftOnly && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Insert)))
		{
			Paste();
		}
		else if (!IsReadOnly() && isShortcut && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_V)))
		{
			Paste();
		}
		else if (isShortcut && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_X)))
		{
			Cut();
		}
		else if (isShiftOnly && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Delete)))
		{
			Cut();
		}
		else if (isShortcut && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_A)))
		{
			SelectAll();
		}
		else if (!IsReadOnly() && !alt && !ctrl && !shift && !super && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Enter)))
		{
			EnterCharacter('\n', false);
		}
		else if (!IsReadOnly() && !alt && !ctrl && !super && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Tab)))
		{
			EnterCharacter('\t', shift);
		}
		
		if (!IsReadOnly() && !io.InputQueueCharacters.empty() && !ctrl && !super)
		{
			for (int i = 0; i < io.InputQueueCharacters.Size; i++)
			{
				auto c = io.InputQueueCharacters[i];
				if (c != 0 && (c == '\n' || c >= 32))
				{
					EnterCharacter(c, shift);
				}
			}

			io.InputQueueCharacters.resize(0);
		}
	}
}

void TextEditor::HandleMouseInputs()
{
	ImGuiIO& io = ImGui::GetIO();
	auto shift = io.KeyShift;
	auto ctrl = io.ConfigMacOSXBehaviors ? io.KeySuper : io.KeyCtrl;
	auto alt = io.ConfigMacOSXBehaviors ? io.KeyCtrl : io.KeyAlt;

	if (ImGui::IsWindowHovered())
	{
		if (!shift && !alt)
		{
			auto click = ImGui::IsMouseClicked(0);
			auto doubleClick = ImGui::IsMouseDoubleClicked(0);
			auto t = ImGui::GetTime();
			auto tripleClick = click && !doubleClick && (mLastClick != -1.0f && (t - mLastClick) < io.MouseDoubleClickTime);

			// Left mouse button triple click
			if (tripleClick)
			{
				if (!ctrl)
				{
					mState.mCursorPosition = mInteractiveStart = mInteractiveEnd = ScreenPosToCoordinates(ImGui::GetMousePos());
					mSelectionMode = SelectionMode::Line;
					SetSelection(mInteractiveStart, mInteractiveEnd, mSelectionMode);
				}

				mLastClick = -1.0f;
			}
			// Left mouse button double click
			else if (doubleClick)
			{
				if (!ctrl)
				{
					mState.mCursorPosition = mInteractiveStart = mInteractiveEnd = ScreenPosToCoordinates(ImGui::GetMousePos());
					if (mSelectionMode == SelectionMode::Line)
					{
						mSelectionMode = SelectionMode::Normal;
					}
					else
					{
						mSelectionMode = SelectionMode::Word;
					}

					SetSelection(mInteractiveStart, mInteractiveEnd, mSelectionMode);
				}

				mLastClick = (float)ImGui::GetTime();
			}
			// Left mouse button click
			else if (click)
			{
				mState.mCursorPosition = mInteractiveStart = mInteractiveEnd = ScreenPosToCoordinates(ImGui::GetMousePos());
				if (ctrl)
				{
					mSelectionMode = SelectionMode::Word;
				}
				else
				{
					mSelectionMode = SelectionMode::Normal;
				}

				SetSelection(mInteractiveStart, mInteractiveEnd, mSelectionMode);

				mLastClick = (float)ImGui::GetTime();
			}
			// Mouse left button dragging (=> update selection)
			else if (ImGui::IsMouseDragging(0) && ImGui::IsMouseDown(0))
			{
				io.WantCaptureMouse = true;
				mState.mCursorPosition = mInteractiveEnd = ScreenPosToCoordinates(ImGui::GetMousePos());
				SetSelection(mInteractiveStart, mInteractiveEnd, mSelectionMode);
			}
		}
	}
}

void TextEditor::Render()
{
	// Compute mCharAdvance regarding to scaled font size (Ctrl + mouse wheel)
	const float fontSize = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, "#", nullptr, nullptr).x;
	mCharAdvance = ImVec2(fontSize, ImGui::GetTextLineHeightWithSpacing() * mLineSpacing);

	// Update palette with the current alpha from style
	for (int i = 0; i < (int)PaletteIndex::Max; ++i)
	{
		auto color = ImGui::ColorConvertU32ToFloat4(mPaletteBase[i]);
		color.w *= ImGui::GetStyle().Alpha;
		mPalette[i] = ImGui::ColorConvertFloat4ToU32(color);
	}

	assert(mLineBuffer.empty());

	auto contentSize = ImGui::GetWindowContentRegionMax();
	auto drawList = ImGui::GetWindowDrawList();
	float longest(mTextStart);

	if (mScrollToTop)
	{
		mScrollToTop = false;
		ImGui::SetScrollY(0.f);
	}

	ImVec2 cursorScreenPos = ImGui::GetCursorScreenPos();
	auto scrollX = ImGui::GetScrollX();
	auto scrollY = ImGui::GetScrollY();

	auto lineNo = (int)floor(scrollY / mCharAdvance.y);
	auto globalLineMax = (int)mLines.size();
	auto lineMax = std::max(0, std::min((int)mLines.size() - 1, lineNo + (int)floor((scrollY + contentSize.y) / mCharAdvance.y)));

	// Deduce mTextStart by evaluating mLines size (global lineMax) plus two spaces as text width
	char buf[16];
	snprintf(buf, 16, " %d ", globalLineMax);
	mTextStart = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, buf, nullptr, nullptr).x + mLeftMargin;

	if (!mLines.empty())
	{
		float spaceSize = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, " ", nullptr, nullptr).x;

		while (lineNo <= lineMax)
		{
			ImVec2 lineStartScreenPos = ImVec2(cursorScreenPos.x, cursorScreenPos.y + lineNo * mCharAdvance.y);
			ImVec2 textScreenPos = ImVec2(lineStartScreenPos.x + mTextStart, lineStartScreenPos.y);

			auto& line = mLines[lineNo];
			longest = std::max(mTextStart + TextDistanceToLineStart(Coordinates(lineNo, GetLineMaxColumn(lineNo))), longest);
			auto columnNo = 0;
			Coordinates lineStartCoord(lineNo, 0);
			Coordinates lineEndCoord(lineNo, GetLineMaxColumn(lineNo));

			// Draw selection for the current line
			float sstart = -1.0f;
			float ssend = -1.0f;

			assert(mState.mSelectionStart <= mState.mSelectionEnd);
			if (mState.mSelectionStart <= lineEndCoord)
			{
				sstart = mState.mSelectionStart > lineStartCoord ? TextDistanceToLineStart(mState.mSelectionStart) : 0.0f;
			}
			if (mState.mSelectionEnd > lineStartCoord)
			{
				ssend = TextDistanceToLineStart(mState.mSelectionEnd < lineEndCoord ? mState.mSelectionEnd : lineEndCoord);
			}

			if (mState.mSelectionEnd.mLine > lineNo)
			{
				ssend += mCharAdvance.x;
			}

			{
				const ImVec2 vstart(lineStartScreenPos.x + mTextStart, lineStartScreenPos.y);
				const ImVec2 vend(lineStartScreenPos.x + mTextStart + TextDistanceToLineStart(lineEndCoord), lineStartScreenPos.y + mCharAdvance.y);
				const static ImU32 textBackgroundColor = 0xD0000000;
				drawList->AddRectFilled(vstart, vend, mPalette[(int)PaletteIndex::Background]);
			}

			if (sstart != -1 && ssend != -1 && sstart < ssend)
			{
				ImVec2 vstart(lineStartScreenPos.x + mTextStart + sstart, lineStartScreenPos.y);
				ImVec2 vend(lineStartScreenPos.x + mTextStart + ssend, lineStartScreenPos.y + mCharAdvance.y);
				drawList->AddRectFilled(vstart, vend, mPalette[(int)PaletteIndex::Selection]);
			}

			// Draw breakpoints
			auto start = ImVec2(lineStartScreenPos.x + scrollX, lineStartScreenPos.y);

			if (mBreakpoints.count(lineNo + 1) != 0)
			{
				auto end = ImVec2(lineStartScreenPos.x + contentSize.x + 2.0f * scrollX, lineStartScreenPos.y + mCharAdvance.y);
				drawList->AddRectFilled(start, end, mPalette[(int)PaletteIndex::Breakpoint]);
			}

			// Draw error markers
			auto errorIt = mErrorMarkers.find(lineNo + 1);
			if (errorIt != mErrorMarkers.end())
			{
				auto end = ImVec2(lineStartScreenPos.x + contentSize.x + 2.0f * scrollX, lineStartScreenPos.y + mCharAdvance.y);
				drawList->AddRectFilled(start, end, mPalette[(int)PaletteIndex::ErrorMarker]);

				if (ImGui::IsMouseHoveringRect(lineStartScreenPos, end))
				{
					ImGui::BeginTooltip();
					ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
					ImGui::Text("Error at line %d:", errorIt->first);
					ImGui::PopStyleColor();
					ImGui::Separator();
					ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.2f, 1.0f));
					ImGui::Text("%s", errorIt->second.c_str());
					ImGui::PopStyleColor();
					ImGui::EndTooltip();
				}
			}

			// Draw line number (right aligned)
			snprintf(buf, 16, "%d  ", lineNo + 1);

			auto lineNoWidth = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, buf, nullptr, nullptr).x;
			drawList->AddText(ImVec2(lineStartScreenPos.x + mTextStart - lineNoWidth, lineStartScreenPos.y), mPalette[(int)PaletteIndex::LineNumber], buf);

			if (mState.mCursorPosition.mLine == lineNo)
			{
				auto focused = ImGui::IsWindowFocused();

				// Highlight the current line (where the cursor is)
				if (!HasSelection())
				{
					auto end = ImVec2(start.x + contentSize.x + scrollX, start.y + mCharAdvance.y);
					drawList->AddRectFilled(start, end, mPalette[(int)(focused ? PaletteIndex::CurrentLineFill : PaletteIndex::CurrentLineFillInactive)]);
					drawList->AddRect(start, end, mPalette[(int)PaletteIndex::CurrentLineEdge], 1.0f);
				}

				// Render the cursor
				if (focused)
				{
					auto timeEnd = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
					auto elapsed = timeEnd - mStartTime;
					if (elapsed > 400)
					{
						float width = 1.0f;
						auto cindex = GetCharacterIndex(mState.mCursorPosition);
						float cx = TextDistanceToLineStart(mState.mCursorPosition);

						if (mOverwrite && cindex < (int)line.size())
						{
							auto c = line[cindex].mChar;
							if (c == '\t')
							{
								auto x = (1.0f + std::floor((1.0f + cx) / (float(mTabSize) * spaceSize))) * (float(mTabSize) * spaceSize);
								width = x - cx;
							}
							else
							{
								char buf2[2];
								buf2[0] = line[cindex].mChar;
								buf2[1] = '\0';
								width = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, buf2).x;
							}
						}

						ImVec2 cstart(textScreenPos.x + cx, lineStartScreenPos.y);
						ImVec2 cend(textScreenPos.x + cx + width, lineStartScreenPos.y + mCharAdvance.y);
						drawList->AddRectFilled(cstart, cend, mPalette[(int)PaletteIndex::Cursor]);
						if (elapsed > 800)
						{
							mStartTime = timeEnd;
						}
					}
				}
			}

			// Render colorized text
			auto prevColor = line.empty() ? mPalette[(int)PaletteIndex::Default] : GetGlyphColor(line[0]);
			ImVec2 bufferOffset;

			for (int i = 0; i < line.size();)
			{
				auto& glyph = line[i];
				auto color = GetGlyphColor(glyph);

				if ((color != prevColor || glyph.mChar == '\t' || glyph.mChar == ' ') && !mLineBuffer.empty())
				{
					const ImVec2 newOffset(textScreenPos.x + bufferOffset.x, textScreenPos.y + bufferOffset.y);
					drawList->AddText(newOffset, prevColor, mLineBuffer.c_str());
					auto textSize = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, mLineBuffer.c_str(), nullptr, nullptr);
					bufferOffset.x += textSize.x;
					mLineBuffer.clear();
				}

				prevColor = color;

				if (glyph.mChar == '\t')
				{
					auto oldX = bufferOffset.x;
					bufferOffset.x = (1.0f + std::floor((1.0f + bufferOffset.x) / (float(mTabSize) * spaceSize))) * (float(mTabSize) * spaceSize);
					++i;

					if (mShowWhitespaces)
					{
						const auto s = ImGui::GetFontSize();
						const auto x1 = textScreenPos.x + oldX + 1.0f;
						const auto x2 = textScreenPos.x + bufferOffset.x - 1.0f;
						const auto y = textScreenPos.y + bufferOffset.y + s * 0.5f;
						const ImVec2 p1(x1, y);
						const ImVec2 p2(x2, y);
						const ImVec2 p3(x2 - s * 0.2f, y - s * 0.2f);
						const ImVec2 p4(x2 - s * 0.2f, y + s * 0.2f);
						drawList->AddLine(p1, p2, 0x90909090);
						drawList->AddLine(p2, p3, 0x90909090);
						drawList->AddLine(p2, p4, 0x90909090);
					}
				}
				else if (glyph.mChar == ' ')
				{
					if (mShowWhitespaces)
					{
						const auto s = ImGui::GetFontSize();
						const auto x = textScreenPos.x + bufferOffset.x + spaceSize * 0.5f;
						const auto y = textScreenPos.y + bufferOffset.y + s * 0.5f;
						drawList->AddCircleFilled(ImVec2(x, y), 1.5f, 0x80808080, 4);
					}
					bufferOffset.x += spaceSize;
					i++;
				}
				else
				{
					auto l = UTF8CharLength(glyph.mChar);
					while (l-- > 0)
					{
						mLineBuffer.push_back(line[i++].mChar);
					}
				}

				++columnNo;
			}

			if (!mLineBuffer.empty())
			{
				const ImVec2 newOffset(textScreenPos.x + bufferOffset.x, textScreenPos.y + bufferOffset.y);
				drawList->AddText(newOffset, prevColor, mLineBuffer.c_str());
				mLineBuffer.clear();
			}

			++lineNo;
		}

		// Draw a tooltip on known identifiers/preprocessor symbols
		if (ImGui::IsMousePosValid())
		{
			auto id = GetWordAt(ScreenPosToCoordinates(ImGui::GetMousePos()));
			if (!id.empty())
			{
				auto it = mLanguageDefinition.mIdentifiers.find(id);
				if (it != mLanguageDefinition.mIdentifiers.end())
				{
					ImGui::BeginTooltip();
					ImGui::TextUnformatted(it->second.mDeclaration.c_str());
					ImGui::EndTooltip();
				}
				else
				{
					auto pi = mLanguageDefinition.mPreprocIdentifiers.find(id);
					if (pi != mLanguageDefinition.mPreprocIdentifiers.end())
					{
						ImGui::BeginTooltip();
						ImGui::TextUnformatted(pi->second.mDeclaration.c_str());
						ImGui::EndTooltip();
					}
				}
			}
		}
	}


	ImGui::Dummy(ImVec2((longest + 2), mLines.size() * mCharAdvance.y));

	if (mScrollToCursor)
	{
		EnsureCursorVisible();
		ImGui::SetWindowFocus();
		mScrollToCursor = false;
	}
}

void TextEditor::Render(const char* aTitle, const ImVec2& aSize, bool aBorder)
{
	mWithinRender = true;
	mTextChanged = false;
	mCursorPositionChanged = false;

	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(mPalette[(int)PaletteIndex::Background]));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
	if (!mIgnoreImGuiChild)
	{
		ImGui::BeginChild(aTitle, aSize, aBorder, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysHorizontalScrollbar | ImGuiWindowFlags_NoMove);
	}

	if (mHandleKeyboardInputs)
	{
		HandleKeyboardInputs();
		ImGui::PushAllowKeyboardFocus(true);
	}

	if (mHandleMouseInputs)
	{
		HandleMouseInputs();
	}

	ColorizeInternal();
	Render();

	if (mHandleKeyboardInputs)
	{
		ImGui::PopAllowKeyboardFocus();
	}

	if (!mIgnoreImGuiChild)
	{
		ImGui::EndChild();
	}

	ImGui::PopStyleVar();
	ImGui::PopStyleColor();

	mWithinRender = false;
}

void TextEditor::SetText(const std::string & aText)
{
	mLines.clear();
	mLines.emplace_back(Line());
	for (auto chr : aText)
	{
		if (chr == '\r')
		{
			// Ignore the carriage return character
		}
		else if (chr == '\n')
		{
			mLines.emplace_back(Line());
		}
		else
		{
			mLines.back().emplace_back(Glyph(chr, PaletteIndex::Default));
		}
	}

	mTextChanged = true;
	mScrollToTop = true;

	mUndoBuffer.clear();
	mUndoIndex = 0;

	Colorize();
}

void TextEditor::SetTextLines(const std::vector<std::string> & aLines)
{
	mLines.clear();

	if (aLines.empty())
	{
		mLines.emplace_back(Line());
	}
	else
	{
		mLines.resize(aLines.size());

		for (size_t i = 0; i < aLines.size(); ++i)
		{
			const std::string & aLine = aLines[i];

			mLines[i].reserve(aLine.size());
			for (size_t j = 0; j < aLine.size(); ++j)
			{
				mLines[i].emplace_back(Glyph(aLine[j], PaletteIndex::Default));
			}
		}
	}

	mTextChanged = true;
	mScrollToTop = true;

	mUndoBuffer.clear();
	mUndoIndex = 0;

	Colorize();
}

void TextEditor::EnterCharacter(ImWchar aChar, bool aShift)
{
	assert(!mReadOnly);

	UndoRecord u;

	u.mBefore = mState;

	if (HasSelection())
	{
		if (aChar == '\t' && mState.mSelectionStart.mLine != mState.mSelectionEnd.mLine)
		{
			auto start = mState.mSelectionStart;
			auto end = mState.mSelectionEnd;
			auto originalEnd = end;

			if (start > end)
				std::swap(start, end);
			start.mColumn = 0;

			if (end.mColumn == 0 && end.mLine > 0)
			{
				--end.mLine;
			}

			if (end.mLine >= (int)mLines.size())
			{
				end.mLine = mLines.empty() ? 0 : (int)mLines.size() - 1;
			}

			end.mColumn = GetLineMaxColumn(end.mLine);

			u.mRemovedStart = start;
			u.mRemovedEnd = end;
			u.mRemoved = GetText(start, end);

			bool modified = false;

			for (int i = start.mLine; i <= end.mLine; i++)
			{
				auto& line = mLines[i];
				if (aShift)
				{
					if (!line.empty())
					{
						if (line.front().mChar == '\t')
						{
							line.erase(line.begin());
							modified = true;
						}
						else
						{
							for (int j = 0; j < mTabSize && !line.empty() && line.front().mChar == ' '; j++)
							{
								line.erase(line.begin());
								modified = true;
							}
						}
					}
				}
				else
				{
					line.insert(line.begin(), Glyph('\t', TextEditor::PaletteIndex::Background));
					modified = true;
				}
			}

			if (modified)
			{
				start = Coordinates(start.mLine, GetCharacterColumn(start.mLine, 0));
				Coordinates rangeEnd;
				if (originalEnd.mColumn != 0)
				{
					end = Coordinates(end.mLine, GetLineMaxColumn(end.mLine));
					rangeEnd = end;
					u.mAdded = GetText(start, end);
				}
				else
				{
					end = Coordinates(originalEnd.mLine, 0);
					rangeEnd = Coordinates(end.mLine - 1, GetLineMaxColumn(end.mLine - 1));
					u.mAdded = GetText(start, rangeEnd);
				}

				u.mAddedStart = start;
				u.mAddedEnd = rangeEnd;
				u.mAfter = mState;

				mState.mSelectionStart = start;
				mState.mSelectionEnd = end;
				AddUndo(u);

				mTextChanged = true;

				EnsureCursorVisible();
			}

			return;
		} // c == '\t'
		else
		{
			u.mRemoved = GetSelectedText();
			u.mRemovedStart = mState.mSelectionStart;
			u.mRemovedEnd = mState.mSelectionEnd;
			DeleteSelection();
		}
	} // HasSelection

	auto coord = GetActualCursorCoordinates();
	u.mAddedStart = coord;

	assert(!mLines.empty());

	if (aChar == '\n')
	{
		InsertLine(coord.mLine + 1);
		auto& line = mLines[coord.mLine];
		auto& newLine = mLines[coord.mLine + 1];

		if (mLanguageDefinition.mAutoIndentation)
		{
			for (size_t it = 0; it < line.size() && isascii(line[it].mChar) && isblank(line[it].mChar); ++it)
			{
				newLine.push_back(line[it]);
			}
		}

		const size_t whitespaceSize = newLine.size();
		auto cindex = GetCharacterIndex(coord);
		newLine.insert(newLine.end(), line.begin() + cindex, line.end());
		line.erase(line.begin() + cindex, line.begin() + line.size());
		SetCursorPosition(Coordinates(coord.mLine + 1, GetCharacterColumn(coord.mLine + 1, (int)whitespaceSize)));
		u.mAdded = (char)aChar;
	}
	else
	{
		char buf[7];
		int e = ImTextCharToUtf8(buf, 7, aChar);
		if (e > 0)
		{
			buf[e] = '\0';
			auto& line = mLines[coord.mLine];
			auto cindex = GetCharacterIndex(coord);

			if (mOverwrite && cindex < (int)line.size())
			{
				auto d = UTF8CharLength(line[cindex].mChar);

				u.mRemovedStart = mState.mCursorPosition;
				u.mRemovedEnd = Coordinates(coord.mLine, GetCharacterColumn(coord.mLine, cindex + d));

				while (d-- > 0 && cindex < (int)line.size())
				{
					u.mRemoved += line[cindex].mChar;
					line.erase(line.begin() + cindex);
				}
			}

			for (auto p = buf; *p != '\0'; p++, ++cindex)
			{
				line.insert(line.begin() + cindex, Glyph(*p, PaletteIndex::Default));
			}

			u.mAdded = buf;

			SetCursorPosition(Coordinates(coord.mLine, GetCharacterColumn(coord.mLine, cindex)));
		}
		else
		{
			return;
		}
	}

	mTextChanged = true;

	u.mAddedEnd = GetActualCursorCoordinates();
	u.mAfter = mState;

	AddUndo(u);

	Colorize(coord.mLine - 1, 3);
	EnsureCursorVisible();
}

void TextEditor::SetReadOnly(bool aValue)
{
	mReadOnly = aValue;
}

void TextEditor::SetColorizerEnable(bool aValue)
{
	mColorizerEnabled = aValue;
}

void TextEditor::SetCursorPosition(const Coordinates & aPosition)
{
	if (mState.mCursorPosition != aPosition)
	{
		mState.mCursorPosition = aPosition;
		mCursorPositionChanged = true;
		EnsureCursorVisible();
	}
}

void TextEditor::SetSelectionStart(const Coordinates & aPosition)
{
	mState.mSelectionStart = SanitizeCoordinates(aPosition);
	if (mState.mSelectionStart > mState.mSelectionEnd)
	{
		std::swap(mState.mSelectionStart, mState.mSelectionEnd);
	}
}

void TextEditor::SetSelectionEnd(const Coordinates & aPosition)
{
	mState.mSelectionEnd = SanitizeCoordinates(aPosition);
	if (mState.mSelectionStart > mState.mSelectionEnd)
	{
		std::swap(mState.mSelectionStart, mState.mSelectionEnd);
	}
}

void TextEditor::SetSelection(const Coordinates & aStart, const Coordinates & aEnd, SelectionMode aMode)
{
	auto oldSelStart = mState.mSelectionStart;
	auto oldSelEnd = mState.mSelectionEnd;

	mState.mSelectionStart = SanitizeCoordinates(aStart);
	mState.mSelectionEnd = SanitizeCoordinates(aEnd);
	if (mState.mSelectionStart > mState.mSelectionEnd)
	{
		std::swap(mState.mSelectionStart, mState.mSelectionEnd);
	}

	switch (aMode)
	{
	case TextEditor::SelectionMode::Normal:
		break;
	case TextEditor::SelectionMode::Word:
	{
		mState.mSelectionStart = FindWordStart(mState.mSelectionStart);
		if (!IsOnWordBoundary(mState.mSelectionEnd))
			mState.mSelectionEnd = FindWordEnd(FindWordStart(mState.mSelectionEnd));
		break;
	}
	case TextEditor::SelectionMode::Line:
	{
		const auto lineNo = mState.mSelectionEnd.mLine;
		const auto lineSize = (size_t)lineNo < mLines.size() ? mLines[lineNo].size() : 0;
		mState.mSelectionStart = Coordinates(mState.mSelectionStart.mLine, 0);
		mState.mSelectionEnd = Coordinates(lineNo, GetLineMaxColumn(lineNo));
		break;
	}
	default:
		break;
	}

	if (mState.mSelectionStart != oldSelStart || mState.mSelectionEnd != oldSelEnd)
	{
		mCursorPositionChanged = true;
	}
}

void TextEditor::SetTabSize(int aValue)
{
	mTabSize = std::max(0, std::min(32, aValue));
}

void TextEditor::InsertText(const std::string & aValue)
{
	InsertText(aValue.c_str());
}

void TextEditor::InsertText(const char * aValue)
{
	if (aValue == nullptr)
	{
		return;
	}

	auto pos = GetActualCursorCoordinates();
	auto start = std::min(pos, mState.mSelectionStart);
	int totalLines = pos.mLine - start.mLine;

	totalLines += InsertTextAt(pos, aValue);

	SetSelection(pos, pos);
	SetCursorPosition(pos);
	Colorize(start.mLine - 1, totalLines + 2);
}

void TextEditor::DeleteSelection()
{
	assert(mState.mSelectionEnd >= mState.mSelectionStart);

	if (mState.mSelectionEnd == mState.mSelectionStart)
	{
		return;
	}

	DeleteRange(mState.mSelectionStart, mState.mSelectionEnd);

	SetSelection(mState.mSelectionStart, mState.mSelectionStart);
	SetCursorPosition(mState.mSelectionStart);
	Colorize(mState.mSelectionStart.mLine, 1);
}

void TextEditor::MoveUp(int aAmount, bool aSelect)
{
	auto oldPos = mState.mCursorPosition;
	mState.mCursorPosition.mLine = std::max(0, mState.mCursorPosition.mLine - aAmount);
	if (oldPos != mState.mCursorPosition)
	{
		if (aSelect)
		{
			if (oldPos == mInteractiveStart)
			{
				mInteractiveStart = mState.mCursorPosition;
			}
			else if (oldPos == mInteractiveEnd)
			{
				mInteractiveEnd = mState.mCursorPosition;
			}
			else
			{
				mInteractiveStart = mState.mCursorPosition;
				mInteractiveEnd = oldPos;
			}
		}
		else
		{
			mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
		}

		SetSelection(mInteractiveStart, mInteractiveEnd);

		EnsureCursorVisible();
	}
}

void TextEditor::MoveDown(int aAmount, bool aSelect)
{
	assert(mState.mCursorPosition.mColumn >= 0);
	auto oldPos = mState.mCursorPosition;
	mState.mCursorPosition.mLine = std::max(0, std::min((int)mLines.size() - 1, mState.mCursorPosition.mLine + aAmount));

	if (mState.mCursorPosition != oldPos)
	{
		if (aSelect)
		{
			if (oldPos == mInteractiveEnd)
			{
				mInteractiveEnd = mState.mCursorPosition;
			}
			else if (oldPos == mInteractiveStart)
			{
				mInteractiveStart = mState.mCursorPosition;
			}
			else
			{
				mInteractiveStart = oldPos;
				mInteractiveEnd = mState.mCursorPosition;
			}
		}
		else
		{
			mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
		}

		SetSelection(mInteractiveStart, mInteractiveEnd);

		EnsureCursorVisible();
	}
}

static bool IsUTFSequence(char c)
{
	return (c & 0xC0) == 0x80;
}

void TextEditor::MoveLeft(int aAmount, bool aSelect, bool aWordMode)
{
	if (mLines.empty())
	{
		return;
	}

	auto oldPos = mState.mCursorPosition;
	mState.mCursorPosition = GetActualCursorCoordinates();
	auto line = mState.mCursorPosition.mLine;
	auto cindex = GetCharacterIndex(mState.mCursorPosition);

	while (aAmount-- > 0)
	{
		if (cindex == 0)
		{
			if (line > 0)
			{
				--line;
				if ((int)mLines.size() > line)
				{
					cindex = (int)mLines[line].size();
				}
				else
				{
					cindex = 0;
				}
			}
		}
		else
		{
			--cindex;
			if (cindex > 0)
			{
				if ((int)mLines.size() > line)
				{
					while (cindex > 0 && IsUTFSequence(mLines[line][cindex].mChar))
					{
						--cindex;
					}
				}
			}
		}

		mState.mCursorPosition = Coordinates(line, GetCharacterColumn(line, cindex));
		if (aWordMode)
		{
			mState.mCursorPosition = FindWordStart(mState.mCursorPosition);
			cindex = GetCharacterIndex(mState.mCursorPosition);
		}
	}

	mState.mCursorPosition = Coordinates(line, GetCharacterColumn(line, cindex));

	assert(mState.mCursorPosition.mColumn >= 0);
	if (aSelect)
	{
		if (oldPos == mInteractiveStart)
		{
			mInteractiveStart = mState.mCursorPosition;
		}
		else if (oldPos == mInteractiveEnd)
		{
			mInteractiveEnd = mState.mCursorPosition;
		}
		else
		{
			mInteractiveStart = mState.mCursorPosition;
			mInteractiveEnd = oldPos;
		}
	}
	else
	{
		mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
	}

	SetSelection(mInteractiveStart, mInteractiveEnd, aSelect && aWordMode ? SelectionMode::Word : SelectionMode::Normal);

	EnsureCursorVisible();
}

void TextEditor::MoveRight(int aAmount, bool aSelect, bool aWordMode)
{
	auto oldPos = mState.mCursorPosition;

	if (mLines.empty() || oldPos.mLine >= mLines.size())
	{
		return;
	}

	auto cindex = GetCharacterIndex(mState.mCursorPosition);
	while (aAmount-- > 0)
	{
		auto lindex = mState.mCursorPosition.mLine;
		auto& line = mLines[lindex];

		if (cindex >= line.size())
		{
			if (mState.mCursorPosition.mLine < mLines.size() - 1)
			{
				mState.mCursorPosition.mLine = std::max(0, std::min((int)mLines.size() - 1, mState.mCursorPosition.mLine + 1));
				mState.mCursorPosition.mColumn = 0;
			}
			else
			{
				return;
			}
		}
		else
		{
			cindex += UTF8CharLength(line[cindex].mChar);
			mState.mCursorPosition = Coordinates(lindex, GetCharacterColumn(lindex, cindex));
			if (aWordMode)
			{
				mState.mCursorPosition = FindNextWord(mState.mCursorPosition);
			}
		}
	}

	if (aSelect)
	{
		if (oldPos == mInteractiveEnd)
		{
			mInteractiveEnd = SanitizeCoordinates(mState.mCursorPosition);
		}
		else if (oldPos == mInteractiveStart)
		{
			mInteractiveStart = mState.mCursorPosition;
		}
		else
		{
			mInteractiveStart = oldPos;
			mInteractiveEnd = mState.mCursorPosition;
		}
	}
	else
	{
		mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
	}

	SetSelection(mInteractiveStart, mInteractiveEnd, aSelect && aWordMode ? SelectionMode::Word : SelectionMode::Normal);

	EnsureCursorVisible();
}

void TextEditor::MoveTop(bool aSelect)
{
	auto oldPos = mState.mCursorPosition;
	SetCursorPosition(Coordinates(0, 0));

	if (mState.mCursorPosition != oldPos)
	{
		if (aSelect)
		{
			mInteractiveEnd = oldPos;
			mInteractiveStart = mState.mCursorPosition;
		}
		else
		{
			mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
		}

		SetSelection(mInteractiveStart, mInteractiveEnd);
	}
}

void TextEditor::TextEditor::MoveBottom(bool aSelect)
{
	auto oldPos = GetCursorPosition();
	auto newPos = Coordinates((int)mLines.size() - 1, 0);
	SetCursorPosition(newPos);
	if (aSelect)
	{
		mInteractiveStart = oldPos;
		mInteractiveEnd = newPos;
	}
	else
	{
		mInteractiveStart = mInteractiveEnd = newPos;
	}

	SetSelection(mInteractiveStart, mInteractiveEnd);
}

void TextEditor::MoveHome(bool aSelect)
{
	auto oldPos = mState.mCursorPosition;
	SetCursorPosition(Coordinates(mState.mCursorPosition.mLine, 0));

	if (mState.mCursorPosition != oldPos)
	{
		if (aSelect)
		{
			if (oldPos == mInteractiveStart)
			{
				mInteractiveStart = mState.mCursorPosition;
			}
			else if (oldPos == mInteractiveEnd)
			{
				mInteractiveEnd = mState.mCursorPosition;
			}
			else
			{
				mInteractiveStart = mState.mCursorPosition;
				mInteractiveEnd = oldPos;
			}
		}
		else
		{
			mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
		}

		SetSelection(mInteractiveStart, mInteractiveEnd);
	}
}

void TextEditor::MoveEnd(bool aSelect)
{
	auto oldPos = mState.mCursorPosition;
	SetCursorPosition(Coordinates(mState.mCursorPosition.mLine, GetLineMaxColumn(oldPos.mLine)));

	if (mState.mCursorPosition != oldPos)
	{
		if (aSelect)
		{
			if (oldPos == mInteractiveEnd)
			{
				mInteractiveEnd = mState.mCursorPosition;
			}
			else if (oldPos == mInteractiveStart)
			{
				mInteractiveStart = mState.mCursorPosition;
			}
			else
			{
				mInteractiveStart = oldPos;
				mInteractiveEnd = mState.mCursorPosition;
			}
		}
		else
		{
			mInteractiveStart = mInteractiveEnd = mState.mCursorPosition;
		}

		SetSelection(mInteractiveStart, mInteractiveEnd);
	}
}

void TextEditor::Delete()
{
	assert(!mReadOnly);

	if (mLines.empty())
	{
		return;
	}

	UndoRecord u;
	u.mBefore = mState;

	if (HasSelection())
	{
		u.mRemoved = GetSelectedText();
		u.mRemovedStart = mState.mSelectionStart;
		u.mRemovedEnd = mState.mSelectionEnd;

		DeleteSelection();
	}
	else
	{
		auto pos = GetActualCursorCoordinates();
		SetCursorPosition(pos);
		auto& line = mLines[pos.mLine];

		if (pos.mColumn == GetLineMaxColumn(pos.mLine))
		{
			if (pos.mLine == (int)mLines.size() - 1)
			{
				return;
			}

			u.mRemoved = '\n';
			u.mRemovedStart = u.mRemovedEnd = GetActualCursorCoordinates();
			Advance(u.mRemovedEnd);

			auto& nextLine = mLines[pos.mLine + 1];
			line.insert(line.end(), nextLine.begin(), nextLine.end());
			RemoveLine(pos.mLine + 1);
		}
		else
		{
			auto cindex = GetCharacterIndex(pos);
			u.mRemovedStart = u.mRemovedEnd = GetActualCursorCoordinates();
			u.mRemovedEnd.mColumn++;
			u.mRemoved = GetText(u.mRemovedStart, u.mRemovedEnd);

			auto d = UTF8CharLength(line[cindex].mChar);
			while (d-- > 0 && cindex < (int)line.size())
			{
				line.erase(line.begin() + cindex);
			}
		}

		mTextChanged = true;

		Colorize(pos.mLine, 1);
	}

	u.mAfter = mState;
	AddUndo(u);
}

void TextEditor::Backspace()
{
	assert(!mReadOnly);

	if (mLines.empty())
	{
		return;
	}

	UndoRecord u;
	u.mBefore = mState;

	if (HasSelection())
	{
		u.mRemoved = GetSelectedText();
		u.mRemovedStart = mState.mSelectionStart;
		u.mRemovedEnd = mState.mSelectionEnd;

		DeleteSelection();
	}
	else
	{
		auto pos = GetActualCursorCoordinates();
		SetCursorPosition(pos);

		if (mState.mCursorPosition.mColumn == 0)
		{
			if (mState.mCursorPosition.mLine == 0)
			{
				return;
			}

			u.mRemoved = '\n';
			u.mRemovedStart = u.mRemovedEnd = Coordinates(pos.mLine - 1, GetLineMaxColumn(pos.mLine - 1));
			Advance(u.mRemovedEnd);

			auto& line = mLines[mState.mCursorPosition.mLine];
			auto& prevLine = mLines[mState.mCursorPosition.mLine - 1];
			auto prevSize = GetLineMaxColumn(mState.mCursorPosition.mLine - 1);
			prevLine.insert(prevLine.end(), line.begin(), line.end());

			ErrorMarkers etmp;
			for (auto& i : mErrorMarkers)
			{
				etmp.insert(ErrorMarkers::value_type(i.first - 1 == mState.mCursorPosition.mLine ? i.first - 1 : i.first, i.second));
			}

			mErrorMarkers = std::move(etmp);

			RemoveLine(mState.mCursorPosition.mLine);
			--mState.mCursorPosition.mLine;
			mState.mCursorPosition.mColumn = prevSize;
		}
		else
		{
			auto& line = mLines[mState.mCursorPosition.mLine];
			auto cindex = GetCharacterIndex(pos) - 1;
			auto cend = cindex + 1;
			while (cindex > 0 && IsUTFSequence(line[cindex].mChar))
			{
				--cindex;
			}

			u.mRemovedStart = u.mRemovedEnd = GetActualCursorCoordinates();
			--u.mRemovedStart.mColumn;

			if (line[cindex].mChar == '\t')
			{
				mState.mCursorPosition.mColumn -= mTabSize;
			}
			else
			{
				--mState.mCursorPosition.mColumn;
			}

			while (cindex < line.size() && cend-- > cindex)
			{
				u.mRemoved += line[cindex].mChar;
				line.erase(line.begin() + cindex);
			}
		}

		mTextChanged = true;

		EnsureCursorVisible();
		Colorize(mState.mCursorPosition.mLine, 1);
	}

	u.mAfter = mState;
	AddUndo(u);
}

void TextEditor::SelectWordUnderCursor()
{
	auto c = GetCursorPosition();
	SetSelection(FindWordStart(c), FindWordEnd(c));
}

void TextEditor::SelectAll()
{
	SetSelection(Coordinates(0, 0), Coordinates((int)mLines.size(), 0));
}

bool TextEditor::HasSelection() const
{
	return mState.mSelectionEnd > mState.mSelectionStart;
}

void TextEditor::Copy()
{
	if (HasSelection())
	{
		ImGui::SetClipboardText(GetSelectedText().c_str());
	}
	else
	{
		if (!mLines.empty())
		{
			std::string str;
			auto& line = mLines[GetActualCursorCoordinates().mLine];
			for (auto& g : line)
			{
				str.push_back(g.mChar);
			}

			ImGui::SetClipboardText(str.c_str());
		}
	}
}

void TextEditor::Cut()
{
	if (IsReadOnly())
	{
		Copy();
	}
	else
	{
		if (HasSelection())
		{
			UndoRecord u;
			u.mBefore = mState;
			u.mRemoved = GetSelectedText();
			u.mRemovedStart = mState.mSelectionStart;
			u.mRemovedEnd = mState.mSelectionEnd;

			Copy();
			DeleteSelection();

			u.mAfter = mState;
			AddUndo(u);
		}
	}
}

void TextEditor::Paste()
{
	if (IsReadOnly())
	{
		return;
	}

	auto clipText = ImGui::GetClipboardText();
	if (clipText != nullptr && strlen(clipText) > 0)
	{
		UndoRecord u;
		u.mBefore = mState;

		if (HasSelection())
		{
			u.mRemoved = GetSelectedText();
			u.mRemovedStart = mState.mSelectionStart;
			u.mRemovedEnd = mState.mSelectionEnd;
			DeleteSelection();
		}

		u.mAdded = clipText;
		u.mAddedStart = GetActualCursorCoordinates();

		InsertText(clipText);

		u.mAddedEnd = GetActualCursorCoordinates();
		u.mAfter = mState;
		AddUndo(u);
	}
}

bool TextEditor::CanUndo() const
{
	return !mReadOnly && mUndoIndex > 0;
}

bool TextEditor::CanRedo() const
{
	return !mReadOnly && mUndoIndex < (int)mUndoBuffer.size();
}

void TextEditor::Undo(int aSteps)
{
	while (CanUndo() && aSteps-- > 0)
	{
		mUndoBuffer[--mUndoIndex].Undo(this);
	}
}

void TextEditor::Redo(int aSteps)
{
	while (CanRedo() && aSteps-- > 0)
	{
		mUndoBuffer[mUndoIndex++].Redo(this);
	}
}

const TextEditor::Palette & TextEditor::GetColorPalette()
{
	const static Palette palette =
	{ {
		0xffc6c8c5, // Default
		0xff6cc8da, // Keyword	
		0xffb7be55, // Number
		0xff5f93de, // String
		0xff74c6f0, // Char literal
		0xffffffff, // Punctuation
		0xffc37fbc, // Preprocessor
		0xffc6c8c5, // Identifier
		0xff6666cc, // Known identifier
		0xffc040a0, // Preproc identifier
		0xff68e8a0, // Comment (single line)
		0xff68e8a0, // Comment (multi line)
		0xd0101010, // Background
		0xffe0e0e0, // Cursor
		0x60b7be55, // Selection
		0x800020ff, // ErrorMarker
		0xff0000ff, // Breakpoint
		0xff707000, // Line number
		0x40000000, // Current line fill
		0x40808080, // Current line fill (inactive)
		0x40a0a0a0, // Current line edge
	} };

	return palette;
}

std::string TextEditor::GetText() const
{
	return GetText(Coordinates(), Coordinates((int)mLines.size(), 0));
}

std::vector<std::string> TextEditor::GetTextLines() const
{
	std::vector<std::string> result;

	result.reserve(mLines.size());

	for (auto & line : mLines)
	{
		std::string text;

		text.resize(line.size());

		for (size_t i = 0; i < line.size(); ++i)
		{
			text[i] = line[i].mChar;
		}

		result.emplace_back(std::move(text));
	}

	return result;
}

std::string TextEditor::GetSelectedText() const
{
	return GetText(mState.mSelectionStart, mState.mSelectionEnd);
}

std::string TextEditor::GetCurrentLineText()const
{
	auto lineLength = GetLineMaxColumn(mState.mCursorPosition.mLine);
	return GetText(Coordinates(mState.mCursorPosition.mLine, 0), Coordinates(mState.mCursorPosition.mLine, lineLength));
}

void TextEditor::ProcessInputs()
{}

void TextEditor::Colorize(int aFromLine, int aLines)
{
	int toLine = aLines == -1 ? (int)mLines.size() : std::min((int)mLines.size(), aFromLine + aLines);
	mColorRangeMin = std::min(mColorRangeMin, aFromLine);
	mColorRangeMax = std::max(mColorRangeMax, toLine);
	mColorRangeMin = std::max(0, mColorRangeMin);
	mColorRangeMax = std::max(mColorRangeMin, mColorRangeMax);
	mCheckComments = true;
}

void TextEditor::ColorizeRange(int aFromLine, int aToLine)
{
	if (mLines.empty() || aFromLine >= aToLine)
	{
		return;
	}

	std::string buffer;
	std::cmatch results;
	std::string id;

	int endLine = std::max(0, std::min((int)mLines.size(), aToLine));
	for (int i = aFromLine; i < endLine; ++i)
	{
		auto& line = mLines[i];

		if (line.empty())
		{
			continue;
		}

		buffer.resize(line.size());
		for (size_t j = 0; j < line.size(); ++j)
		{
			auto& col = line[j];
			buffer[j] = col.mChar;
			col.mColorIndex = PaletteIndex::Default;
		}

		const char * bufferBegin = &buffer.front();
		const char * bufferEnd = bufferBegin + buffer.size();

		auto last = bufferEnd;

		for (auto first = bufferBegin; first != last; )
		{
			const char * token_begin = nullptr;
			const char * token_end = nullptr;
			PaletteIndex token_color = PaletteIndex::Default;

			bool hasTokenizeResult = false;

			if (mLanguageDefinition.mTokenize != nullptr)
			{
				if (mLanguageDefinition.mTokenize(first, last, token_begin, token_end, token_color))
				{
					hasTokenizeResult = true;
				}
			}

			if (hasTokenizeResult == false)
			{
				for (auto& p : mRegexList)
				{
					if (std::regex_search(first, last, results, p.first, std::regex_constants::match_continuous))
					{
						hasTokenizeResult = true;

						auto& v = *results.begin();
						token_begin = v.first;
						token_end = v.second;
						token_color = p.second;
						break;
					}
				}
			}

			if (hasTokenizeResult == false)
			{
				first++;
			}
			else
			{
				const size_t token_length = token_end - token_begin;

				if (token_color == PaletteIndex::Identifier)
				{
					id.assign(token_begin, token_end);

					// todo : allmost all language definitions use lower case to specify keywords, so shouldn't this use ::tolower ?
					if (!mLanguageDefinition.mCaseSensitive)
					{
						std::transform(id.begin(), id.end(), id.begin(), ::toupper);
					}

					if (!line[first - bufferBegin].mPreprocessor)
					{
						if (mLanguageDefinition.mKeywords.count(id) != 0)
						{
							token_color = PaletteIndex::Keyword;
						}
						else if (mLanguageDefinition.mIdentifiers.count(id) != 0)
						{
							token_color = PaletteIndex::KnownIdentifier;
						}
						else if (mLanguageDefinition.mPreprocIdentifiers.count(id) != 0)
						{
							token_color = PaletteIndex::PreprocIdentifier;
						}
					}
					else
					{
						if (mLanguageDefinition.mPreprocIdentifiers.count(id) != 0)
						{
							token_color = PaletteIndex::PreprocIdentifier;
						}
					}
				}

				for (size_t j = 0; j < token_length; ++j)
				{
					line[(token_begin - bufferBegin) + j].mColorIndex = token_color;
				}

				first = token_end;
			}
		}
	}
}

void TextEditor::ColorizeInternal()
{
	if (mLines.empty() || !mColorizerEnabled)
	{
		return;
	}

	if (mCheckComments)
	{
		auto endLine = mLines.size();
		auto endIndex = 0;
		auto commentStartLine = endLine;
		auto commentStartIndex = endIndex;
		auto withinString = false;
		auto withinSingleLineComment = false;
		auto withinPreproc = false;
		auto firstChar = true; // there is no other non-whitespace characters in the line before
		auto concatenate = false; // '\' on the very end of the line
		auto currentLine = 0;
		auto currentIndex = 0;
		while (currentLine < endLine || currentIndex < endIndex)
		{
			auto& line = mLines[currentLine];

			if (currentIndex == 0 && !concatenate)
			{
				withinSingleLineComment = false;
				withinPreproc = false;
				firstChar = true;
			}

			concatenate = false;

			if (!line.empty())
			{
				auto& g = line[currentIndex];
				auto c = g.mChar;

				if (c != mLanguageDefinition.mPreprocChar && !isspace(c))
				{
					firstChar = false;
				}

				if (currentIndex == (int)line.size() - 1 && line[line.size() - 1].mChar == '\\')
				{
					concatenate = true;
				}

				bool inComment = (commentStartLine < currentLine || (commentStartLine == currentLine && commentStartIndex <= currentIndex));

				if (withinString)
				{
					line[currentIndex].mMultiLineComment = inComment;

					if (c == '\"')
					{
						if (currentIndex + 1 < (int)line.size() && line[currentIndex + 1].mChar == '\"')
						{
							currentIndex += 1;
							if (currentIndex < (int)line.size())
								line[currentIndex].mMultiLineComment = inComment;
						}
						else
						{
							withinString = false;
						}
					}
					else if (c == '\\')
					{
						currentIndex += 1;
						if (currentIndex < (int)line.size())
						{
							line[currentIndex].mMultiLineComment = inComment;
						}
					}
				}
				else
				{
					if (firstChar && c == mLanguageDefinition.mPreprocChar)
					{
						withinPreproc = true;
					}

					if (c == '\"')
					{
						withinString = true;
						line[currentIndex].mMultiLineComment = inComment;
					}
					else
					{
						auto pred = [](const char& a, const Glyph& b) { return a == b.mChar; };
						auto from = line.begin() + currentIndex;
						auto& startStr = mLanguageDefinition.mCommentStart;
						auto& singleStartStr = mLanguageDefinition.mSingleLineComment;

						if (singleStartStr.size() > 0 &&
							currentIndex + singleStartStr.size() <= line.size() &&
							equals(singleStartStr.begin(), singleStartStr.end(), from, from + singleStartStr.size(), pred))
						{
							withinSingleLineComment = true;
						}
						else if (!withinSingleLineComment && currentIndex + startStr.size() <= line.size() &&
							equals(startStr.begin(), startStr.end(), from, from + startStr.size(), pred))
						{
							commentStartLine = currentLine;
							commentStartIndex = currentIndex;
						}

						inComment = inComment = (commentStartLine < currentLine || (commentStartLine == currentLine && commentStartIndex <= currentIndex));

						line[currentIndex].mMultiLineComment = inComment;
						line[currentIndex].mComment = withinSingleLineComment;

						auto& endStr = mLanguageDefinition.mCommentEnd;
						if (currentIndex + 1 >= (int)endStr.size() &&
							equals(endStr.begin(), endStr.end(), from + 1 - endStr.size(), from + 1, pred))
						{
							commentStartIndex = endIndex;
							commentStartLine = endLine;
						}
					}
				}

				line[currentIndex].mPreprocessor = withinPreproc;
				currentIndex += UTF8CharLength(c);
				if (currentIndex >= (int)line.size())
				{
					currentIndex = 0;
					++currentLine;
				}
			}
			else
			{
				currentIndex = 0;
				++currentLine;
			}
		}

		mCheckComments = false;
	}

	if (mColorRangeMin < mColorRangeMax)
	{
		const int increment = (mLanguageDefinition.mTokenize == nullptr) ? 10 : 10000;
		const int to = std::min(mColorRangeMin + increment, mColorRangeMax);
		ColorizeRange(mColorRangeMin, to);
		mColorRangeMin = to;

		if (mColorRangeMax == mColorRangeMin)
		{
			mColorRangeMin = std::numeric_limits<int>::max();
			mColorRangeMax = 0;
		}
	}
}

float TextEditor::TextDistanceToLineStart(const Coordinates& aFrom) const
{
	auto& line = mLines[aFrom.mLine];
	float distance = 0.0f;
	float spaceSize = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, " ", nullptr, nullptr).x;
	int colIndex = GetCharacterIndex(aFrom);
	for (size_t it = 0u; it < line.size() && it < colIndex; )
	{
		if (line[it].mChar == '\t')
		{
			distance = (1.0f + std::floor((1.0f + distance) / (float(mTabSize) * spaceSize))) * (float(mTabSize) * spaceSize);
			++it;
		}
		else
		{
			auto d = UTF8CharLength(line[it].mChar);
			char tempCString[7];
			int i = 0;
			for (; i < 6 && d-- > 0 && it < (int)line.size(); i++, it++)
			{
				tempCString[i] = line[it].mChar;
			}

			tempCString[i] = '\0';
			distance += ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, tempCString, nullptr, nullptr).x;
		}
	}

	return distance;
}

void TextEditor::EnsureCursorVisible()
{
	if (!mWithinRender)
	{
		mScrollToCursor = true;
		return;
	}

	float scrollX = ImGui::GetScrollX();
	float scrollY = ImGui::GetScrollY();

	auto height = ImGui::GetWindowHeight();
	auto width = ImGui::GetWindowWidth();

	auto top = 1 + (int)ceil(scrollY / mCharAdvance.y);
	auto bottom = (int)ceil((scrollY + height) / mCharAdvance.y);

	auto left = (int)ceil(scrollX / mCharAdvance.x);
	auto right = (int)ceil((scrollX + width) / mCharAdvance.x);

	auto pos = GetActualCursorCoordinates();
	auto len = TextDistanceToLineStart(pos);

	if (pos.mLine < top)
	{
		ImGui::SetScrollY(std::max(0.0f, (pos.mLine - 1) * mCharAdvance.y));
	}
	
	if (pos.mLine > bottom - 4)
	{
		ImGui::SetScrollY(std::max(0.0f, (pos.mLine + 4) * mCharAdvance.y - height));
	}
	
	if (len + mTextStart < left + 4)
	{
		ImGui::SetScrollX(std::max(0.0f, len + mTextStart - 4));
	}
	
	if (len + mTextStart > right - 4)
	{
		ImGui::SetScrollX(std::max(0.0f, len + mTextStart + 4 - width));
	}
}

int TextEditor::GetPageSize() const
{
	auto height = ImGui::GetWindowHeight() - 20.0f;
	return (int)floor(height / mCharAdvance.y);
}

TextEditor::UndoRecord::UndoRecord(
	const std::string& aAdded,
	const TextEditor::Coordinates aAddedStart,
	const TextEditor::Coordinates aAddedEnd,
	const std::string& aRemoved,
	const TextEditor::Coordinates aRemovedStart,
	const TextEditor::Coordinates aRemovedEnd,
	TextEditor::EditorState& aBefore,
	TextEditor::EditorState& aAfter)
	: mAdded(aAdded)
	, mAddedStart(aAddedStart)
	, mAddedEnd(aAddedEnd)
	, mRemoved(aRemoved)
	, mRemovedStart(aRemovedStart)
	, mRemovedEnd(aRemovedEnd)
	, mBefore(aBefore)
	, mAfter(aAfter)
{
	assert(mAddedStart <= mAddedEnd);
	assert(mRemovedStart <= mRemovedEnd);
}

void TextEditor::UndoRecord::Undo(TextEditor * aEditor)
{
	if (!mAdded.empty())
	{
		aEditor->DeleteRange(mAddedStart, mAddedEnd);
		aEditor->Colorize(mAddedStart.mLine - 1, mAddedEnd.mLine - mAddedStart.mLine + 2);
	}

	if (!mRemoved.empty())
	{
		auto start = mRemovedStart;
		aEditor->InsertTextAt(start, mRemoved.c_str());
		aEditor->Colorize(mRemovedStart.mLine - 1, mRemovedEnd.mLine - mRemovedStart.mLine + 2);
	}

	aEditor->mState = mBefore;
	aEditor->EnsureCursorVisible();
}

void TextEditor::UndoRecord::Redo(TextEditor * aEditor)
{
	if (!mRemoved.empty())
	{
		aEditor->DeleteRange(mRemovedStart, mRemovedEnd);
		aEditor->Colorize(mRemovedStart.mLine - 1, mRemovedEnd.mLine - mRemovedStart.mLine + 1);
	}

	if (!mAdded.empty())
	{
		auto start = mAddedStart;
		aEditor->InsertTextAt(start, mAdded.c_str());
		aEditor->Colorize(mAddedStart.mLine - 1, mAddedEnd.mLine - mAddedStart.mLine + 1);
	}

	aEditor->mState = mAfter;
	aEditor->EnsureCursorVisible();
}

const TextEditor::LanguageDefinition& TextEditor::LanguageDefinition::HLSL()
{
	static bool initialized = false;
	static LanguageDefinition langDef;

	if (!initialized)
	{
		// TODO-milkru: Find VK HLSL specific keywords and identifiers and update these lists.
		static const char* const keywords[] = {
			"AppendStructuredBuffer", "asm", "asm_fragment", "BlendState", "bool", "break", "Buffer", "ByteAddressBuffer", "case", "cbuffer", "centroid", "class", "column_major", "compile", "compile_fragment",
			"CompileShader", "const", "continue", "ComputeShader", "ConsumeStructuredBuffer", "default", "DepthStencilState", "DepthStencilView", "discard", "do", "double", "DomainShader", "dword", "else",
			"export", "extern", "false", "float", "for", "fxgroup", "GeometryShader", "groupshared", "half", "Hullshader", "if", "in", "inline", "inout", "InputPatch", "int", "interface", "line", "lineadj",
			"linear", "LineStream", "matrix", "min16float", "min10float", "min16int", "min12int", "min16uint", "namespace", "nointerpolation", "noperspective", "NULL", "out", "OutputPatch", "packoffset",
			"pass", "pixelfragment", "PixelShader", "point", "PointStream", "precise", "RasterizerState", "RenderTargetView", "return", "register", "row_major", "RWBuffer", "RWByteAddressBuffer", "RWStructuredBuffer",
			"RWTexture1D", "RWTexture1DArray", "RWTexture2D", "RWTexture2DArray", "RWTexture3D", "sample", "sampler", "SamplerState", "SamplerComparisonState", "shared", "snorm", "stateblock", "stateblock_state",
			"static", "string", "struct", "switch", "StructuredBuffer", "tbuffer", "technique", "technique10", "technique11", "texture", "Texture1D", "Texture1DArray", "Texture2D", "Texture2DArray", "Texture2DMS",
			"Texture2DMSArray", "Texture3D", "TextureCube", "TextureCubeArray", "true", "typedef", "triangle", "triangleadj", "TriangleStream", "uint", "uniform", "unorm", "unsigned", "vector", "vertexfragment",
			"VertexShader", "void", "volatile", "while",
			"bool1","bool2","bool3","bool4","double1","double2","double3","double4", "float1", "float2", "float3", "float4", "int1", "int2", "int3", "int4", "in", "out", "inout",
			"uint1", "uint2", "uint3", "uint4", "dword1", "dword2", "dword3", "dword4", "half1", "half2", "half3", "half4",
			"float1x1","float2x1","float3x1","float4x1","float1x2","float2x2","float3x2","float4x2", "float1x3","float2x3","float3x3","float4x3","float1x4","float2x4","float3x4","float4x4",
			"half1x1","half2x1","half3x1","half4x1","half1x2","half2x2","half3x2","half4x2", "half1x3","half2x3","half3x3","half4x3","half1x4","half2x4","half3x4","half4x4",
		};

		for (const auto& keyword : keywords)
		{
			langDef.mKeywords.insert(keyword);
		}

		{
			langDef.mIdentifiers.insert(std::make_pair("abort", Identifier("Terminates the current draw or dispatch call being executed.")));
			langDef.mIdentifiers.insert(std::make_pair("abs", Identifier("Absolute value (per component).")));
			langDef.mIdentifiers.insert(std::make_pair("acos", Identifier("Returns the arccosine of each component of x.")));
			langDef.mIdentifiers.insert(std::make_pair("all", Identifier("Test if all components of x are nonzero.")));
			langDef.mIdentifiers.insert(std::make_pair("AllMemoryBarrier", Identifier("Blocks execution of all threads in a group until all memory accesses have been completed.")));
			langDef.mIdentifiers.insert(std::make_pair("AllMemoryBarrierWithGroupSync", Identifier("Blocks execution of all threads in a group until all memory accesses have been completed and all threads in the group have reached this call.")));
			langDef.mIdentifiers.insert(std::make_pair("any", Identifier("Test if any component of x is nonzero.")));
			langDef.mIdentifiers.insert(std::make_pair("asdouble", Identifier("Reinterprets a cast value into a double.")));
			langDef.mIdentifiers.insert(std::make_pair("asfloat", Identifier("Convert the input type to a float.")));
			langDef.mIdentifiers.insert(std::make_pair("asin", Identifier("Returns the arcsine of each component of x.")));
			langDef.mIdentifiers.insert(std::make_pair("asint", Identifier("Convert the input type to an integer.")));
			langDef.mIdentifiers.insert(std::make_pair("asuint", Identifier("Convert the input type to an unsigned integer.")));
			langDef.mIdentifiers.insert(std::make_pair("atan", Identifier("Returns the arctangent of x.")));
			langDef.mIdentifiers.insert(std::make_pair("atan2", Identifier("Returns the arctangent of of two values (x,y).")));
			langDef.mIdentifiers.insert(std::make_pair("ceil", Identifier("Returns the smallest integer which is greater than or equal to x.")));
			langDef.mIdentifiers.insert(std::make_pair("CheckAccessFullyMapped", Identifier("Determines whether all values from a Sample or Load operation accessed mapped tiles in a tiled resource.")));
			langDef.mIdentifiers.insert(std::make_pair("clamp", Identifier("Clamps x to the range [min, max].")));
			langDef.mIdentifiers.insert(std::make_pair("clip", Identifier("Discards the current pixel, if any component of x is less than zero.")));
			langDef.mIdentifiers.insert(std::make_pair("cos", Identifier("Returns the cosine of x.")));
			langDef.mIdentifiers.insert(std::make_pair("cosh", Identifier("Returns the hyperbolic cosine of x.")));
			langDef.mIdentifiers.insert(std::make_pair("countbits", Identifier("Counts the number of bits (per component) in the input integer.")));
			langDef.mIdentifiers.insert(std::make_pair("cross", Identifier("Returns the cross product of two 3D vectors.")));
			langDef.mIdentifiers.insert(std::make_pair("D3DCOLORtoUBYTE4", Identifier("Swizzles and scales components of the 4D vector x to compensate for the lack of UBYTE4 support in some hardware.")));
			langDef.mIdentifiers.insert(std::make_pair("ddx", Identifier("Returns the partial derivative of x with respect to the screen-space x-coordinate.")));
			langDef.mIdentifiers.insert(std::make_pair("ddx_coarse", Identifier("Computes a low precision partial derivative with respect to the screen-space x-coordinate.")));
			langDef.mIdentifiers.insert(std::make_pair("ddx_fine", Identifier("Computes a high precision partial derivative with respect to the screen-space x-coordinate.")));
			langDef.mIdentifiers.insert(std::make_pair("ddy", Identifier("Returns the partial derivative of x with respect to the screen-space y-coordinate.")));
			langDef.mIdentifiers.insert(std::make_pair("ddy_coarse", Identifier("Returns the partial derivative of x with respect to the screen-space y-coordinate.")));
			langDef.mIdentifiers.insert(std::make_pair("ddy_fine", Identifier("Computes a high precision partial derivative with respect to the screen-space y-coordinate.")));
			langDef.mIdentifiers.insert(std::make_pair("degrees", Identifier("Converts x from radians to degrees.")));
			langDef.mIdentifiers.insert(std::make_pair("determinant", Identifier("Returns the determinant of the square matrix m.")));
			langDef.mIdentifiers.insert(std::make_pair("DeviceMemoryBarrier", Identifier("Blocks execution of all threads in a group until all device memory accesses have been completed.")));
			langDef.mIdentifiers.insert(std::make_pair("DeviceMemoryBarrierWithGroupSync", Identifier("Blocks execution of all threads in a group until all device memory accesses have been completed and all threads in the group have reached this call.")));
			langDef.mIdentifiers.insert(std::make_pair("distance", Identifier("Returns the distance between two points.")));
			langDef.mIdentifiers.insert(std::make_pair("dot", Identifier("Returns the dot product of two vectors.")));
			langDef.mIdentifiers.insert(std::make_pair("dst", Identifier("Calculates a distance vector.")));
			langDef.mIdentifiers.insert(std::make_pair("errorf", Identifier("Submits an error message to the information queue.")));
			langDef.mIdentifiers.insert(std::make_pair("EvaluateAttributeAtCentroid", Identifier("Evaluates at the pixel centroid.")));
			langDef.mIdentifiers.insert(std::make_pair("EvaluateAttributeAtSample", Identifier("Evaluates at the indexed sample location.")));
			langDef.mIdentifiers.insert(std::make_pair("EvaluateAttributeSnapped", Identifier("Evaluates at the pixel centroid with an offset.")));
			langDef.mIdentifiers.insert(std::make_pair("exp", Identifier("Returns the base-e exponent.")));
			langDef.mIdentifiers.insert(std::make_pair("exp2", Identifier("Base 2 exponent(per component).")));
			langDef.mIdentifiers.insert(std::make_pair("f16tof32", Identifier("Converts the float16 stored in the low-half of the uint to a float.")));
			langDef.mIdentifiers.insert(std::make_pair("f32tof16", Identifier("Converts an input into a float16 type.")));
			langDef.mIdentifiers.insert(std::make_pair("faceforward", Identifier("Returns -n * sign(dot(i, ng)).")));
			langDef.mIdentifiers.insert(std::make_pair("firstbithigh", Identifier("Gets the location of the first set bit starting from the highest order bit and working downward, per component.")));
			langDef.mIdentifiers.insert(std::make_pair("firstbitlow", Identifier("Returns the location of the first set bit starting from the lowest order bit and working upward, per component.")));
			langDef.mIdentifiers.insert(std::make_pair("floor", Identifier("Returns the greatest integer which is less than or equal to x.")));
			langDef.mIdentifiers.insert(std::make_pair("fma", Identifier("Returns the double-precision fused multiply-addition of a * b + c.")));
			langDef.mIdentifiers.insert(std::make_pair("fmod", Identifier("Returns the floating point remainder of x/y.")));
			langDef.mIdentifiers.insert(std::make_pair("frac", Identifier("Returns the fractional part of x.")));
			langDef.mIdentifiers.insert(std::make_pair("frexp", Identifier("Returns the mantissa and exponent of x.")));
			langDef.mIdentifiers.insert(std::make_pair("fwidth", Identifier("Returns abs(ddx(x)) + abs(ddy(x))")));
			langDef.mIdentifiers.insert(std::make_pair("GetRenderTargetSampleCount", Identifier("Returns the number of render-target samples.")));
			langDef.mIdentifiers.insert(std::make_pair("GetRenderTargetSamplePosition", Identifier("Returns a sample position (x,y) for a given sample index.")));
			langDef.mIdentifiers.insert(std::make_pair("GroupMemoryBarrier", Identifier("Blocks execution of all threads in a group until all group shared accesses have been completed.")));
			langDef.mIdentifiers.insert(std::make_pair("GroupMemoryBarrierWithGroupSync", Identifier("Blocks execution of all threads in a group until all group shared accesses have been completed and all threads in the group have reached this call.")));
			langDef.mIdentifiers.insert(std::make_pair("InterlockedAdd", Identifier("Performs a guaranteed atomic add of value to the dest resource variable.")));
			langDef.mIdentifiers.insert(std::make_pair("InterlockedAnd", Identifier("Performs a guaranteed atomic and.")));
			langDef.mIdentifiers.insert(std::make_pair("InterlockedCompareExchange", Identifier("Atomically compares the input to the comparison value and exchanges the result.")));
			langDef.mIdentifiers.insert(std::make_pair("InterlockedCompareStore", Identifier("Atomically compares the input to the comparison value.")));
			langDef.mIdentifiers.insert(std::make_pair("InterlockedExchange", Identifier("Assigns value to dest and returns the original value.")));
			langDef.mIdentifiers.insert(std::make_pair("InterlockedMax", Identifier("Performs a guaranteed atomic max.")));
			langDef.mIdentifiers.insert(std::make_pair("InterlockedMin", Identifier("Performs a guaranteed atomic min.")));
			langDef.mIdentifiers.insert(std::make_pair("InterlockedOr", Identifier("Performs a guaranteed atomic or.")));
			langDef.mIdentifiers.insert(std::make_pair("InterlockedXor", Identifier("Performs a guaranteed atomic xor.")));
			langDef.mIdentifiers.insert(std::make_pair("isfinite", Identifier("Returns true if x is finite, false otherwise.")));
			langDef.mIdentifiers.insert(std::make_pair("isinf", Identifier("Returns true if x is +INF or -INF, false otherwise.")));
			langDef.mIdentifiers.insert(std::make_pair("isnan", Identifier("Returns true if x is NAN or QNAN, false otherwise.")));
			langDef.mIdentifiers.insert(std::make_pair("ldexp", Identifier("Returns x * 2exp")));
			langDef.mIdentifiers.insert(std::make_pair("length", Identifier("Returns the length of the vector v.")));
			langDef.mIdentifiers.insert(std::make_pair("lerp", Identifier("Returns x + s(y - x).")));
			langDef.mIdentifiers.insert(std::make_pair("lit", Identifier("Returns a lighting vector (ambient, diffuse, specular, 1)")));
			langDef.mIdentifiers.insert(std::make_pair("log", Identifier("Returns the base-e logarithm of x.")));
			langDef.mIdentifiers.insert(std::make_pair("log10", Identifier("Returns the base-10 logarithm of x.")));
			langDef.mIdentifiers.insert(std::make_pair("log2", Identifier("Returns the base - 2 logarithm of x.")));
			langDef.mIdentifiers.insert(std::make_pair("mad", Identifier("Performs an arithmetic multiply/add operation on three values.")));
			langDef.mIdentifiers.insert(std::make_pair("max", Identifier("Selects the greater of x and y.")));
			langDef.mIdentifiers.insert(std::make_pair("min", Identifier("Selects the lesser of x and y.")));
			langDef.mIdentifiers.insert(std::make_pair("modf", Identifier("Splits the value x into fractional and integer parts.")));
			langDef.mIdentifiers.insert(std::make_pair("msad4", Identifier("Compares a 4-byte reference value and an 8-byte source value and accumulates a vector of 4 sums.")));
			langDef.mIdentifiers.insert(std::make_pair("mul", Identifier("Performs matrix multiplication using x and y.")));
			langDef.mIdentifiers.insert(std::make_pair("noise", Identifier("Generates a random value using the Perlin-noise algorithm.")));
			langDef.mIdentifiers.insert(std::make_pair("normalize", Identifier("Returns a normalized vector.")));
			langDef.mIdentifiers.insert(std::make_pair("pow", Identifier("Returns x^n.")));
			langDef.mIdentifiers.insert(std::make_pair("printf", Identifier("Submits a custom shader message to the information queue.")));
			langDef.mIdentifiers.insert(std::make_pair("Process2DQuadTessFactorsAvg", Identifier("Generates the corrected tessellation factors for a quad patch.")));
			langDef.mIdentifiers.insert(std::make_pair("Process2DQuadTessFactorsMax", Identifier("Generates the corrected tessellation factors for a quad patch.")));
			langDef.mIdentifiers.insert(std::make_pair("Process2DQuadTessFactorsMin", Identifier("Generates the corrected tessellation factors for a quad patch.")));
			langDef.mIdentifiers.insert(std::make_pair("ProcessIsolineTessFactors", Identifier("Generates the rounded tessellation factors for an isoline.")));
			langDef.mIdentifiers.insert(std::make_pair("ProcessQuadTessFactorsAvg", Identifier("Generates the corrected tessellation factors for a quad patch.")));
			langDef.mIdentifiers.insert(std::make_pair("ProcessQuadTessFactorsMax", Identifier("Generates the corrected tessellation factors for a quad patch.")));
			langDef.mIdentifiers.insert(std::make_pair("ProcessQuadTessFactorsMin", Identifier("Generates the corrected tessellation factors for a quad patch.")));
			langDef.mIdentifiers.insert(std::make_pair("ProcessTriTessFactorsAvg", Identifier("Generates the corrected tessellation factors for a tri patch.")));
			langDef.mIdentifiers.insert(std::make_pair("ProcessTriTessFactorsMax", Identifier("Generates the corrected tessellation factors for a tri patch.")));
			langDef.mIdentifiers.insert(std::make_pair("ProcessTriTessFactorsMin", Identifier("Generates the corrected tessellation factors for a tri patch.")));
			langDef.mIdentifiers.insert(std::make_pair("radians", Identifier("Converts x from degrees to radians.")));
			langDef.mIdentifiers.insert(std::make_pair("rcp", Identifier("Calculates a fast, approximate, per-component reciprocal.")));
			langDef.mIdentifiers.insert(std::make_pair("reflect", Identifier("Returns a reflection vector.")));
			langDef.mIdentifiers.insert(std::make_pair("refract", Identifier("Returns the refraction vector.")));
			langDef.mIdentifiers.insert(std::make_pair("reversebits", Identifier("Reverses the order of the bits, per component.")));
			langDef.mIdentifiers.insert(std::make_pair("round", Identifier("Rounds x to the nearest integer")));
			langDef.mIdentifiers.insert(std::make_pair("rsqrt", Identifier("Returns 1 / sqrt(x)")));
			langDef.mIdentifiers.insert(std::make_pair("saturate", Identifier("Clamps x to the range [0, 1]")));
			langDef.mIdentifiers.insert(std::make_pair("sign", Identifier("Computes the sign of x.")));
			langDef.mIdentifiers.insert(std::make_pair("sin", Identifier("Returns the sine of x")));
			langDef.mIdentifiers.insert(std::make_pair("sincos", Identifier("Returns the sineand cosine of x.")));
			langDef.mIdentifiers.insert(std::make_pair("sinh", Identifier("Returns the hyperbolic sine of x")));
			langDef.mIdentifiers.insert(std::make_pair("smoothstep", Identifier("Returns a smooth Hermite interpolation between 0 and 1.")));
			langDef.mIdentifiers.insert(std::make_pair("sqrt", Identifier("Square root (per component)")));
			langDef.mIdentifiers.insert(std::make_pair("step", Identifier("Returns (x >= a) ? 1 : 0")));
			langDef.mIdentifiers.insert(std::make_pair("tan", Identifier("Returns the tangent of x")));
			langDef.mIdentifiers.insert(std::make_pair("tanh", Identifier("Returns the hyperbolic tangent of x")));
			langDef.mIdentifiers.insert(std::make_pair("tex1D", Identifier("1D texture lookup.")));
			langDef.mIdentifiers.insert(std::make_pair("tex1Dbias", Identifier("1D texture lookup with bias.")));
			langDef.mIdentifiers.insert(std::make_pair("tex1Dgrad", Identifier("1D texture lookup with a gradient.")));
			langDef.mIdentifiers.insert(std::make_pair("tex1Dlod", Identifier("1D texture lookup with LOD.")));
			langDef.mIdentifiers.insert(std::make_pair("tex1Dproj", Identifier("1D texture lookup with projective divide.")));
			langDef.mIdentifiers.insert(std::make_pair("tex2D", Identifier("2D texture lookup.")));
			langDef.mIdentifiers.insert(std::make_pair("tex2Dbias", Identifier("2D texture lookup with bias.")));
			langDef.mIdentifiers.insert(std::make_pair("tex2Dgrad", Identifier("2D texture lookup with a gradient.")));
			langDef.mIdentifiers.insert(std::make_pair("tex2Dlod", Identifier("2D texture lookup with LOD.")));
			langDef.mIdentifiers.insert(std::make_pair("tex2Dproj", Identifier("2D texture lookup with projective divide.")));
			langDef.mIdentifiers.insert(std::make_pair("tex3D", Identifier("3D texture lookup.")));
			langDef.mIdentifiers.insert(std::make_pair("tex3Dbias", Identifier("3D texture lookup with bias.")));
			langDef.mIdentifiers.insert(std::make_pair("tex3Dgrad", Identifier("3D texture lookup with a gradient.")));
			langDef.mIdentifiers.insert(std::make_pair("tex3Dlod", Identifier("3D texture lookup with LOD.")));
			langDef.mIdentifiers.insert(std::make_pair("tex3Dproj", Identifier("3D texture lookup with projective divide.")));
			langDef.mIdentifiers.insert(std::make_pair("texCUBE", Identifier("Cube texture lookup.")));
			langDef.mIdentifiers.insert(std::make_pair("texCUBEbias", Identifier("Cube texture lookup with bias.")));
			langDef.mIdentifiers.insert(std::make_pair("texCUBEgrad", Identifier("Cube texture lookup with a gradient.")));
			langDef.mIdentifiers.insert(std::make_pair("texCUBElod", Identifier("Cube texture lookup with LOD.")));
			langDef.mIdentifiers.insert(std::make_pair("texCUBEproj", Identifier("Cube texture lookup with projective divide.")));
			langDef.mIdentifiers.insert(std::make_pair("transpose", Identifier("Returns the transpose of the matrix m.")));
			langDef.mIdentifiers.insert(std::make_pair("trunc", Identifier("Truncates floating-point value(s) to integer value(s)")));
		}

		{
			langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("[ \\t]*#[ \\t]*[a-zA-Z_]+", PaletteIndex::Preprocessor));
			langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("L?\\\"(\\\\.|[^\\\"])*\\\"", PaletteIndex::String));
			langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("\\'\\\\?[^\\']\\'", PaletteIndex::CharLiteral));
			langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)([eE][+-]?[0-9]+)?[fF]?", PaletteIndex::Number));
			langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("[+-]?[0-9]+[Uu]?[lL]?[lL]?", PaletteIndex::Number));
			langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("0[0-7]+[Uu]?[lL]?[lL]?", PaletteIndex::Number));
			langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("0[xX][0-9a-fA-F]+[uU]?[lL]?[lL]?", PaletteIndex::Number));
			langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("[a-zA-Z_][a-zA-Z0-9_]*", PaletteIndex::Identifier));
			langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("[\\[\\]\\{\\}\\!\\%\\^\\&\\*\\(\\)\\-\\+\\=\\~\\|\\<\\>\\?\\/\\;\\,\\.]", PaletteIndex::Punctuation));
		}

		langDef.mCommentStart = "/*";
		langDef.mCommentEnd = "*/";
		langDef.mSingleLineComment = "//";

		langDef.mCaseSensitive = true;
		langDef.mAutoIndentation = true;

		langDef.mName = "HLSL";

		initialized = true;
	}

	return langDef;
}

const TextEditor::LanguageDefinition& TextEditor::LanguageDefinition::GLSL()
{
	static bool initialized = false;
	static LanguageDefinition langDef;

	if (!initialized)
	{
		// TODO-milkru: Find VK GLSL specific keywords and identifiers and update these lists.
		static const char* const keywords[] = {
			"auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else", "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long", "register", "restrict", "return", "short",
			"signed", "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned", "void", "volatile", "while", "_Alignas", "_Alignof", "_Atomic", "_Bool", "_Complex", "_Generic", "_Imaginary",
			"_Noreturn", "_Static_assert", "_Thread_local", "attribute", "uniform", "varying", "layout", "centroid", "flat", "smooth", "noperspective", "patch", "sample", "subroutine", "in", "out", "inout",
			"bool", "true", "false", "invariant", "mat2", "mat3", "mat4", "dmat2", "dmat3", "dmat4", "mat2x2", "mat2x3", "mat2x4", "dmat2x2", "dmat2x3", "dmat2x4", "mat3x2", "mat3x3", "mat3x4", "dmat3x2", "dmat3x3", "dmat3x4",
			"mat4x2", "mat4x3", "mat4x4", "dmat4x2", "dmat4x3", "dmat4x4", "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4", "bvec2", "bvec3", "bvec4", "dvec2", "dvec3", "dvec4", "uint", "uvec2", "uvec3", "uvec4",
			"lowp", "mediump", "highp", "precision", "sampler1D", "sampler2D", "sampler3D", "samplerCube", "sampler1DShadow", "sampler2DShadow", "samplerCubeShadow", "sampler1DArray", "sampler2DArray", "sampler1DArrayShadow",
			"sampler2DArrayShadow", "isampler1D", "isampler2D", "isampler3D", "isamplerCube", "isampler1DArray", "isampler2DArray", "usampler1D", "usampler2D", "usampler3D", "usamplerCube", "usampler1DArray", "usampler2DArray",
			"sampler2DRect", "sampler2DRectShadow", "isampler2DRect", "usampler2DRect", "samplerBuffer", "isamplerBuffer", "usamplerBuffer", "sampler2DMS", "isampler2DMS", "usampler2DMS", "sampler2DMSArray", "isampler2DMSArray",
			"usampler2DMSArray", "samplerCubeArray", "samplerCubeArrayShadow", "isamplerCubeArray", "usamplerCubeArray", "shared", "writeonly", "readonly", "image2D", "image1D", "image3D"
		};

		for (const auto& keyword : keywords)
		{
			langDef.mKeywords.insert(keyword);
		}

		{
			langDef.mIdentifiers.insert(std::make_pair("radians", Identifier("genType radians(genType degrees)\nConverts x from degrees to radians.")));
			langDef.mIdentifiers.insert(std::make_pair("degrees", Identifier("genType degrees(genType radians)\nConverts x from radians to degrees.")));
			langDef.mIdentifiers.insert(std::make_pair("sin", Identifier("genType sin(genType angle)\nReturns the sine of x")));
			langDef.mIdentifiers.insert(std::make_pair("cos", Identifier("genType cos(genType angle)\nReturns the cosine of x.")));
			langDef.mIdentifiers.insert(std::make_pair("tan", Identifier("genType tan(genType angle)\nReturns the tangent of x")));
			langDef.mIdentifiers.insert(std::make_pair("asin", Identifier("genType asin(genType x)\nReturns the arcsine of each component of x.")));
			langDef.mIdentifiers.insert(std::make_pair("acos", Identifier("genType acos(genType x)\nReturns the arccosine of each component of x.")));
			langDef.mIdentifiers.insert(std::make_pair("atan", Identifier("genType atan(genType y, genType x)\ngenType atan(genType y_over_x)\nReturns the arctangent of x.")));
			langDef.mIdentifiers.insert(std::make_pair("sinh", Identifier("genType sinh(genType x)\nReturns the hyperbolic sine of x")));
			langDef.mIdentifiers.insert(std::make_pair("cosh", Identifier("genType cosh(genType x)\nReturns the hyperbolic cosine of x.")));
			langDef.mIdentifiers.insert(std::make_pair("tanh", Identifier("genType tanh(genType x)\nReturns the hyperbolic tangent of x")));
			langDef.mIdentifiers.insert(std::make_pair("asinh", Identifier("genType asinh(genType x)\nReturns the arc hyperbolic sine of x")));
			langDef.mIdentifiers.insert(std::make_pair("acosh", Identifier("genType acosh(genType x)\nReturns the arc hyperbolic cosine of x.")));
			langDef.mIdentifiers.insert(std::make_pair("atanh", Identifier("genType atanh(genType x)\nReturns the arc hyperbolic tangent of x")));
			langDef.mIdentifiers.insert(std::make_pair("pow", Identifier("genType pow(genType x, genType n)\nReturns x^n.")));
			langDef.mIdentifiers.insert(std::make_pair("exp", Identifier("genType exp(genType x)\nReturns the base-e exponent.")));
			langDef.mIdentifiers.insert(std::make_pair("exp2", Identifier("genType exp2(genType x)\nBase 2 exponent(per component).")));
			langDef.mIdentifiers.insert(std::make_pair("log", Identifier("genType log(genType x)\nReturns the base-e logarithm of x.")));
			langDef.mIdentifiers.insert(std::make_pair("log2", Identifier("genType log2(genType x)\nReturns the base - 2 logarithm of x.")));
			langDef.mIdentifiers.insert(std::make_pair("sqrt", Identifier("genType sqrt(genType x)\ngenDType sqrt(genDType x)\nSquare root (per component).")));
			langDef.mIdentifiers.insert(std::make_pair("inversesqrt", Identifier("genType inversesqrt(genType x)\ngenDType inversesqrt(genDType x)\nReturns rcp(sqrt(x)).")));
			langDef.mIdentifiers.insert(std::make_pair("abs", Identifier("genType abs(genType x)\ngenIType abs(genIType x)\ngenDType abs(genDType x)\nAbsolute value (per component).")));
			langDef.mIdentifiers.insert(std::make_pair("sign", Identifier("genType sign(genType x)\ngenIType sign(genIType x)\ngenDType sign(genDType x)\nComputes the sign of x.")));
			langDef.mIdentifiers.insert(std::make_pair("floor", Identifier("genType floor(genType x)\ngenDType floor(genDType x)\nReturns the greatest integer which is less than or equal to x.")));
			langDef.mIdentifiers.insert(std::make_pair("trunc", Identifier("genType trunc(genType x)\ngenDType trunc(genDType x)\nTruncates floating-point value(s) to integer value(s)")));
			langDef.mIdentifiers.insert(std::make_pair("round", Identifier("genType round(genType x)\ngenDType round(genDType x)\nRounds x to the nearest integer")));
			langDef.mIdentifiers.insert(std::make_pair("roundEven", Identifier("genType roundEven(genType x)\ngenDType roundEven(genDType x)\nReturns a value equal to the nearest integer to x. A fractional part of 0.5 will round toward the nearest even integer.")));
			langDef.mIdentifiers.insert(std::make_pair("ceil", Identifier("genType ceil(genType x)\ngenDType ceil(genDType x)\nReturns the smallest integer which is greater than or equal to x.")));
			langDef.mIdentifiers.insert(std::make_pair("fract", Identifier("genType fract(genType x)\ngenDType fract(genDType x)\nReturns the fractional part of x.")));
			langDef.mIdentifiers.insert(std::make_pair("mod", Identifier("genType mod(genType x, float y)\ngenType mod(genType x, genType y)\ngenDType mod(genDType x, double y)\ngenDType mod(genDType x, genDType y)\nModulus.Returns x – y * floor(x / y).")));
			langDef.mIdentifiers.insert(std::make_pair("modf", Identifier("genType modf(genType x, out genType i)\ngenDType modf(genDType x, out genDType i)\nSplits the value x into fractional and integer parts.")));
			langDef.mIdentifiers.insert(std::make_pair("max", Identifier("genType max(genType x, genType y)\ngenType max(genType x, float y)\nSelects the greater of x and y.")));
			langDef.mIdentifiers.insert(std::make_pair("min", Identifier("genType min(genType x, genType y)\ngenType min(genType x, float y)\nSelects the lesser of x and y.")));
			langDef.mIdentifiers.insert(std::make_pair("clamp", Identifier("genType clamp(genType x, genType minVal, genType maxVal)\ngenType clamp(genType x, float minVal, float maxVal)\nClamps x to the range [min, max].")));
			langDef.mIdentifiers.insert(std::make_pair("mix", Identifier("genType mix(genType x, genType y, genType a)\ngenType mix(genType x, genType y, float a)\nReturns x*(1-a)+y*a.")));
			langDef.mIdentifiers.insert(std::make_pair("isinf", Identifier("genBType isinf(genType x)\ngenBType isinf(genDType x)\nReturns true if x is +INF or -INF, false otherwise.")));
			langDef.mIdentifiers.insert(std::make_pair("isnan", Identifier("genBType isnan(genType x)\ngenBType isnan(genDType x)\nReturns true if x is NAN or QNAN, false otherwise.")));
			langDef.mIdentifiers.insert(std::make_pair("smoothstep", Identifier("genType smoothstep(genType edge0, genType edge1, genType x)\ngenType smoothstep(float edge0, float edge1, genType x)\nReturns a smooth Hermite interpolation between 0 and 1.")));
			langDef.mIdentifiers.insert(std::make_pair("step", Identifier("genType step(genType edge, genType x)\ngenType step(float edge, genType x)\nReturns (x >= a) ? 1 : 0")));
			langDef.mIdentifiers.insert(std::make_pair("floatBitsToInt", Identifier("genIType floatBitsToInt(genType x)\nReturns a signed or unsigned integer value representing the encoding of a floating-point value. The floatingpoint value's bit-level representation is preserved.")));
			langDef.mIdentifiers.insert(std::make_pair("floatBitsToUint", Identifier("genUType floatBitsToUint(genType x)\nReturns a signed or unsigned integer value representing the encoding of a floating-point value. The floatingpoint value's bit-level representation is preserved.")));
			langDef.mIdentifiers.insert(std::make_pair("intBitsToFloat", Identifier("genType intBitsToFloat(genIType x)\nReturns a floating-point value corresponding to a signed or unsigned integer encoding of a floating-point value.")));
			langDef.mIdentifiers.insert(std::make_pair("uintBitsToFloat", Identifier("genType uintBitsToFloat(genUType x)\nReturns a floating-point value corresponding to a signed or unsigned integer encoding of a floating-point value.")));
			langDef.mIdentifiers.insert(std::make_pair("fmod", Identifier("Returns the floating point remainder of x/y.")));
			langDef.mIdentifiers.insert(std::make_pair("fma", Identifier("genType fma(genType a, genType b, genType c)\nReturns the double-precision fused multiply-addition of a * b + c.")));
			langDef.mIdentifiers.insert(std::make_pair("ldexp", Identifier("genType ldexp(genType x, genIType exp)\nReturns x * 2exp")));
			langDef.mIdentifiers.insert(std::make_pair("packUnorm2x16", Identifier("uint packUnorm2x16(vec2 v)\nFirst, converts each component of the normalized floating - point value v into 8 or 16bit integer values. Then, the results are packed into the returned 32bit unsigned integer.")));
			langDef.mIdentifiers.insert(std::make_pair("packUnorm4x8", Identifier("uint packUnorm4x8(vec4 v)\nFirst, converts each component of the normalized floating - point value v into 8 or 16bit integer values. Then, the results are packed into the returned 32bit unsigned integer.")));
			langDef.mIdentifiers.insert(std::make_pair("packSnorm4x8", Identifier("uint packUnorm4x8(vec4 v)\nFirst, converts each component of the normalized floating - point value v into 8 or 16bit integer values. Then, the results are packed into the returned 32bit unsigned integer.")));
			langDef.mIdentifiers.insert(std::make_pair("unpackUnorm2x16", Identifier("vec2 unpackUnorm2x16(uint p)\nFirst, unpacks a single 32bit unsigned integer p into a pair of 16bit unsigned integers, four 8bit unsigned integers, or four 8bit signed integers.Then, each component is converted to a normalized floating point value to generate the returned two or four component vector.")));
			langDef.mIdentifiers.insert(std::make_pair("unpackUnorm4x8", Identifier("vec4 unpackUnorm4x8(uint p)\nFirst, unpacks a single 32bit unsigned integer p into a pair of 16bit unsigned integers, four 8bit unsigned integers, or four 8bit signed integers.Then, each component is converted to a normalized floating point value to generate the returned two or four component vector.")));
			langDef.mIdentifiers.insert(std::make_pair("unpackSnorm4x8", Identifier("vec4 unpackSnorm4x8(uint p)\nFirst, unpacks a single 32bit unsigned integer p into a pair of 16bit unsigned integers, four 8bit unsigned integers, or four 8bit signed integers.Then, each component is converted to a normalized floating point value to generate the returned two or four component vector.")));
			langDef.mIdentifiers.insert(std::make_pair("packDouble2x32", Identifier("double packDouble2x32(uvec2 v)\nReturns a double-precision value obtained by packing the components of v into a 64-bit value.")));
			langDef.mIdentifiers.insert(std::make_pair("unpackDouble2x32", Identifier("uvec2 unpackDouble2x32(double d)\nReturns a two-component unsigned integer vector representation of v.")));
			langDef.mIdentifiers.insert(std::make_pair("length", Identifier("float length(genType x)\nReturns the length of the vector v.")));
			langDef.mIdentifiers.insert(std::make_pair("distance", Identifier("float distance(genType p0, genType p1)\nReturns the distance between two points.")));
			langDef.mIdentifiers.insert(std::make_pair("dot", Identifier("float dot(genType x, genType y)\nReturns the dot product of two vectors.")));
			langDef.mIdentifiers.insert(std::make_pair("cross", Identifier("vec3 cross(vec3 x, vec3 y)\nReturns the cross product of two 3D vectors.")));
			langDef.mIdentifiers.insert(std::make_pair("normalize", Identifier("genType normalize(genType v)\nReturns a normalized vector.")));
			langDef.mIdentifiers.insert(std::make_pair("faceforward", Identifier("genType faceforward(genType N, genType I, genType Nref)\nReturns -n * sign(dot(i, ng)).")));
			langDef.mIdentifiers.insert(std::make_pair("reflect", Identifier("genType reflect(genType I, genType N)\nReturns a reflection vector.")));
			langDef.mIdentifiers.insert(std::make_pair("refract", Identifier("genType refract(genType I, genType N, float eta)\nReturns the refraction vector.")));
			langDef.mIdentifiers.insert(std::make_pair("matrixCompMult", Identifier("mat matrixCompMult(mat x, mat y)\nMultiply matrix x by matrix y component-wise.")));
			langDef.mIdentifiers.insert(std::make_pair("outerProduct", Identifier("Linear algebraic matrix multiply c * r.")));
			langDef.mIdentifiers.insert(std::make_pair("transpose", Identifier("mat transpose(mat m)\nReturns the transpose of the matrix m.")));
			langDef.mIdentifiers.insert(std::make_pair("determinant", Identifier("float determinant(mat m)\nReturns the determinant of the square matrix m.")));
			langDef.mIdentifiers.insert(std::make_pair("inverse", Identifier("mat inverse(mat m)\nReturns a matrix that is the inverse of m.")));
			langDef.mIdentifiers.insert(std::make_pair("lessThan", Identifier("bvec lessThan(vec x, vec y)\nReturns the component-wise compare of x < y")));
			langDef.mIdentifiers.insert(std::make_pair("lessThanEqual", Identifier("bvec lessThanEqual(vec x, vec y)\nReturns the component-wise compare of x <= y")));
			langDef.mIdentifiers.insert(std::make_pair("greaterThan", Identifier("bvec greaterThan(vec x, vec y)\nReturns the component-wise compare of x > y")));
			langDef.mIdentifiers.insert(std::make_pair("greaterThanEqual", Identifier("bvec greaterThanEqual(vec x, vec y)\nReturns the component-wise compare of x >= y")));
			langDef.mIdentifiers.insert(std::make_pair("equal", Identifier("bvec equal(vec x, vec y)\nReturns the component-wise compare of x == y")));
			langDef.mIdentifiers.insert(std::make_pair("notEqual", Identifier("bvec notEqual(vec x, vec y)\nReturns the component-wise compare of x != y")));
			langDef.mIdentifiers.insert(std::make_pair("any", Identifier("bool any(bvec x)\nTest if any component of x is nonzero.")));
			langDef.mIdentifiers.insert(std::make_pair("all", Identifier("bool all(bvec x)\nTest if all components of x are nonzero.")));
			langDef.mIdentifiers.insert(std::make_pair("not", Identifier("bvec not(bvec x)\nReturns the component-wise logical complement of x.")));
			langDef.mIdentifiers.insert(std::make_pair("uaddCarry", Identifier("genUType uaddCarry(genUType x, genUType y, out genUType carry)\nAdds 32bit unsigned integer x and y, returning the sum modulo 2^32.")));
			langDef.mIdentifiers.insert(std::make_pair("usubBorrow", Identifier("genUType usubBorrow(genUType x, genUType y, out genUType borrow)\nSubtracts the 32bit unsigned integer y from x, returning the difference if non-negatice, or 2^32 plus the difference otherwise.")));
			langDef.mIdentifiers.insert(std::make_pair("umulExtended", Identifier("void umulExtended(genUType x, genUType y, out genUType msb, out genUType lsb)\nMultiplies 32bit integers x and y, producing a 64bit result.")));
			langDef.mIdentifiers.insert(std::make_pair("imulExtended", Identifier("void imulExtended(genIType x, genIType y, out genIType msb, out genIType lsb)\nMultiplies 32bit integers x and y, producing a 64bit result.")));
			langDef.mIdentifiers.insert(std::make_pair("bitfieldExtract", Identifier("genIType bitfieldExtract(genIType value, int offset, int bits)\ngenUType bitfieldExtract(genUType value, int offset, int bits)\nExtracts bits [offset, offset + bits - 1] from value, returning them in the least significant bits of the result.")));
			langDef.mIdentifiers.insert(std::make_pair("bitfieldInsert", Identifier("genIType bitfieldInsert(genIType base, genIType insert, int offset, int bits)\ngenUType bitfieldInsert(genUType base, genUType insert, int offset, int bits)\nReturns the insertion the bits leas-significant bits of insert into base")));
			langDef.mIdentifiers.insert(std::make_pair("bitfieldReverse", Identifier("genIType bitfieldReverse(genIType value)\ngenUType bitfieldReverse(genUType value)\nReturns the reversal of the bits of value.")));
			langDef.mIdentifiers.insert(std::make_pair("bitCount", Identifier("genIType bitCount(genIType value)\ngenUType bitCount(genUType value)\nReturns the number of bits set to 1 in the binary representation of value.")));
			langDef.mIdentifiers.insert(std::make_pair("findLSB", Identifier("genIType findLSB(genIType value)\ngenUType findLSB(genUType value)\nReturns the bit number of the least significant bit set to 1 in the binary representation of value.")));
			langDef.mIdentifiers.insert(std::make_pair("findMSB", Identifier("genIType findMSB(genIType value)\ngenUType findMSB(genUType value)\nReturns the bit number of the most significant bit in the binary representation of value.")));
			langDef.mIdentifiers.insert(std::make_pair("textureSize", Identifier("ivecX textureSize(gsamplerXD sampler, int lod)\nReturns the dimensions of level lod  (if present) for the texture bound to sample.")));
			langDef.mIdentifiers.insert(std::make_pair("textureQueryLod", Identifier("vec2 textureQueryLod(gsamplerXD sampler, vecX P)\nReturns the mipmap array(s) that would be accessed in the x component of the return value.")));
			langDef.mIdentifiers.insert(std::make_pair("texture", Identifier("gvec4 texture(gsamplerXD sampler, vecX P, [float bias])\nUse the texture coordinate P to do a texture lookup in the texture currently bound to sampler.")));
			langDef.mIdentifiers.insert(std::make_pair("textureProj", Identifier("Do a texture lookup with projection.")));
			langDef.mIdentifiers.insert(std::make_pair("textureLod", Identifier("gvec4 textureLod(gsamplerXD sampler, vecX P, float lod)\nDo a texture lookup as in texture but with explicit LOD.")));
			langDef.mIdentifiers.insert(std::make_pair("textureOffset", Identifier("gvec4 textureOffset(gsamplerXD sampler, vecX P, ivecX offset, [float bias])\nDo a texture lookup as in texture but with offset added to the (u,v,w) texel coordinates before looking up each texel.")));
			langDef.mIdentifiers.insert(std::make_pair("texelFetch", Identifier("gvec4 texelFetch(gsamplerXD sampler, ivecX P, int lod)\nUse integer texture coordinate P to lookup a single texel from sampler.")));
			langDef.mIdentifiers.insert(std::make_pair("texelFetchOffset", Identifier("gvec4 texelFetchOffset(gsamplerXD sampler, ivecX P, int lod, int offset)\nFetch a single texel as in texelFetch offset by offset.")));
			langDef.mIdentifiers.insert(std::make_pair("textureProjLod", Identifier("Do a projective texture lookup with explicit LOD.")));
			langDef.mIdentifiers.insert(std::make_pair("textureLodOffset", Identifier("gvec4 textureLodOffset(gsamplerXD sampler, vecX P, float lod, ivecX offset)\nDo an offset texture lookup with explicit LOD.")));
			langDef.mIdentifiers.insert(std::make_pair("textureProjLodOffset", Identifier("Do an offset projective texture lookup with explicit LOD.")));
			langDef.mIdentifiers.insert(std::make_pair("textureGrad", Identifier("gvec4 textureGrad(gsamplerXD sampler, vecX P, vecX dPdx, vecX dPdy)\nDo a texture lookup as in texture but with explicit gradients.")));
			langDef.mIdentifiers.insert(std::make_pair("textureGradOffset", Identifier("gvec4 textureGradOffset(gsamplerXD sampler, vecX P, vecX dPdx, vecX dPdy, ivecX offset)\nDo a texture lookup with both explicit gradient and offset, as described in textureGrad and textureOffset.")));
			langDef.mIdentifiers.insert(std::make_pair("textureProjGrad", Identifier("Do a texture lookup both projectively and with explicit gradient.")));
			langDef.mIdentifiers.insert(std::make_pair("textureProjGradOffset", Identifier("Do a texture lookup both projectively and with explicit gradient as well as with offset.")));
			langDef.mIdentifiers.insert(std::make_pair("textureGather", Identifier("gvec4 textureGather(gsampler2D sampler, vec2 P, [int comp])\nGathers four texels from a texture")));
			langDef.mIdentifiers.insert(std::make_pair("textureGatherOffset", Identifier("gvec4 textureGatherOffset(gsampler2D sampler, vec2 P, ivec2 offset, [int comp])\nGathers four texels from a texture with offset.")));
			langDef.mIdentifiers.insert(std::make_pair("textureGatherOffsets", Identifier("gvec4 textureGatherOffsets(gsampler2D sampler, vec2 P, ivec2 offsets[4], [int comp])\nGathers four texels from a texture with an array of offsets.")));
			langDef.mIdentifiers.insert(std::make_pair("texture1D", Identifier("1D texture lookup.")));
			langDef.mIdentifiers.insert(std::make_pair("texture1DLod", Identifier("1D texture lookup with LOD.")));
			langDef.mIdentifiers.insert(std::make_pair("texture1DProj", Identifier("1D texture lookup with projective divide.")));
			langDef.mIdentifiers.insert(std::make_pair("texture1DProjLod", Identifier("1D texture lookup with projective divide and with LOD.")));
			langDef.mIdentifiers.insert(std::make_pair("texture2D", Identifier("2D texture lookup.")));
			langDef.mIdentifiers.insert(std::make_pair("texture2DLod", Identifier("2D texture lookup with LOD.")));
			langDef.mIdentifiers.insert(std::make_pair("texture2DProj", Identifier("2D texture lookup with projective divide.")));
			langDef.mIdentifiers.insert(std::make_pair("texture2DProjLod", Identifier("2D texture lookup with projective divide and with LOD.")));
			langDef.mIdentifiers.insert(std::make_pair("texture3D", Identifier("3D texture lookup.")));
			langDef.mIdentifiers.insert(std::make_pair("texture3DLod", Identifier("3D texture lookup with LOD.")));
			langDef.mIdentifiers.insert(std::make_pair("texture3DProj", Identifier("3D texture lookup with projective divide.")));
			langDef.mIdentifiers.insert(std::make_pair("texture3DProjLod", Identifier("3D texture lookup with projective divide and with LOD.")));
			langDef.mIdentifiers.insert(std::make_pair("textureCube", Identifier("Cube texture lookup.")));
			langDef.mIdentifiers.insert(std::make_pair("textureCubeLod", Identifier("Cube texture lookup with LOD.")));
			langDef.mIdentifiers.insert(std::make_pair("shadow1D", Identifier("1D texture lookup.")));
			langDef.mIdentifiers.insert(std::make_pair("shadow1DLod", Identifier("1D texture lookup with LOD.")));
			langDef.mIdentifiers.insert(std::make_pair("shadow1DProj", Identifier("1D texture lookup with projective divide.")));
			langDef.mIdentifiers.insert(std::make_pair("shadow1DProjLod", Identifier("1D texture lookup with projective divide and with LOD.")));
			langDef.mIdentifiers.insert(std::make_pair("shadow2D", Identifier("2D texture lookup.")));
			langDef.mIdentifiers.insert(std::make_pair("shadow2DLod", Identifier("2D texture lookup with LOD.")));
			langDef.mIdentifiers.insert(std::make_pair("shadow2DProj", Identifier("2D texture lookup with projective divide.")));
			langDef.mIdentifiers.insert(std::make_pair("shadow2DProjLod", Identifier("2D texture lookup with projective divide and with LOD.")));
			langDef.mIdentifiers.insert(std::make_pair("dFdx", Identifier("genType dFdx(genType p)\nReturns the partial derivative of x with respect to the screen-space x-coordinate.")));
			langDef.mIdentifiers.insert(std::make_pair("dFdy", Identifier("genType dFdy(genType p)\nReturns the partial derivative of x with respect to the screen-space y-coordinate.")));
			langDef.mIdentifiers.insert(std::make_pair("fwidth", Identifier("genType fwidth(genType p)\nReturns abs(ddx(x)) + abs(ddy(x))")));
			langDef.mIdentifiers.insert(std::make_pair("interpolateAtCentroid", Identifier("Return the value of the input varying interpolant sampled at a location inside the both the pixel and the primitive being processed.")));
			langDef.mIdentifiers.insert(std::make_pair("interpolateAtSample", Identifier("Return the value of the input varying interpolant at the location of sample number sample.")));
			langDef.mIdentifiers.insert(std::make_pair("interpolateAtOffset", Identifier("Return the value of the input varying interpolant sampled at an offset from the center of the pixel specified by offset.")));
			langDef.mIdentifiers.insert(std::make_pair("noise1", Identifier("Generates a random value")));
			langDef.mIdentifiers.insert(std::make_pair("noise2", Identifier("Generates a random value")));
			langDef.mIdentifiers.insert(std::make_pair("noise3", Identifier("Generates a random value")));
			langDef.mIdentifiers.insert(std::make_pair("noise4", Identifier("Generates a random value")));
			langDef.mIdentifiers.insert(std::make_pair("EmitStreamVertex", Identifier("void EmitStreamVertex(int stream)\nEmit the current values of output variables to the current output primitive on stream stream.")));
			langDef.mIdentifiers.insert(std::make_pair("EndStreamPrimitive", Identifier("void EndStreamPrimitive(int stream)\nCompletes the current output primitive on stream stream and starts a new one.")));
			langDef.mIdentifiers.insert(std::make_pair("EmitVertex", Identifier("void EmitVertex()\nEmit the current values to the current output primitive.")));
			langDef.mIdentifiers.insert(std::make_pair("EndPrimitive", Identifier("void EndPrimitive()\nCompletes the current output primitive and starts a new one.")));
			langDef.mIdentifiers.insert(std::make_pair("barrier", Identifier("void barrier()\nSynchronize execution of multiple shader invocations")));
			langDef.mIdentifiers.insert(std::make_pair("groupMemoryBarrier", Identifier("void groupMemoryBarrier()\nControls the ordering of memory transaction issued shader invocation relative to a work group")));
			langDef.mIdentifiers.insert(std::make_pair("memoryBarrier", Identifier("uint memoryBarrier()\nControls the ordering of memory transactions issued by a single shader invocation")));
			langDef.mIdentifiers.insert(std::make_pair("memoryBarrierAtomicCounter", Identifier("void memoryBarrierAtomicCounter()\nControls the ordering of operations on atomic counters issued by a single shader invocation")));
			langDef.mIdentifiers.insert(std::make_pair("memoryBarrierBuffer", Identifier("void memoryBarrierBuffer()\nControls the ordering of operations on buffer variables issued by a single shader invocation")));
			langDef.mIdentifiers.insert(std::make_pair("memoryBarrierImage", Identifier("void memoryBarrierImage()\nControls the ordering of operations on image variables issued by a single shader invocation")));
			langDef.mIdentifiers.insert(std::make_pair("memoryBarrierShared", Identifier("void memoryBarrierShared()\nControls the ordering of operations on shared variables issued by a single shader invocation")));
			langDef.mIdentifiers.insert(std::make_pair("atomicAdd", Identifier("int atomicAdd(inout int mem, int data)\nuint atomicAdd(inout uint mem, uint data)\nPerform an atomic addition to a variable")));
			langDef.mIdentifiers.insert(std::make_pair("atomicAnd", Identifier("int atomicAnd(inout int mem, int data)\nuint atomicAnd(inout uint mem, uint data)\nPerform an atomic logical AND operation to a variable")));
			langDef.mIdentifiers.insert(std::make_pair("atomicCompSwap", Identifier("int atomicCompSwap(inout int mem, uint compare, uint data)\nuint atomicCompSwap(inout uint mem, uint compare, uint data)\nPerform an atomic compare-exchange operation to a variable")));
			langDef.mIdentifiers.insert(std::make_pair("atomicCounter", Identifier("uint atomicCounter(atomic_uint c)\nReturn the current value of an atomic counter")));
			langDef.mIdentifiers.insert(std::make_pair("atomicCounterDecrement", Identifier("uint atomicCounterDecrement(atomic_uint c)\nAtomically decrement a counter and return its new value")));
			langDef.mIdentifiers.insert(std::make_pair("atomicCounterIncrement", Identifier("uint atomicCounterIncrement(atomic_uint c)\nAtomically increment a counter and return the prior value")));
			langDef.mIdentifiers.insert(std::make_pair("atomicExchange", Identifier("int atomicExchange(inout int mem, int data)\nuint atomicExchange(inout uint mem, uint data)\nPerform an atomic exchange operation to a variable ")));
			langDef.mIdentifiers.insert(std::make_pair("atomicMax", Identifier("int atomicMax(inout int mem, int data)\nuint atomicMax(inout uint mem, uint data)\nPerform an atomic max operation to a variable")));
			langDef.mIdentifiers.insert(std::make_pair("atomicMin", Identifier("int atomicMin(inout int mem, int data)\nuint atomicMin(inout uint mem, uint data)\nPerform an atomic min operation to a variable ")));
			langDef.mIdentifiers.insert(std::make_pair("atomicOr", Identifier("int atomicOr(inout int mem, int data)\nuint atomicOr(inout uint mem, uint data)\nPerform an atomic logical OR operation to a variable")));
			langDef.mIdentifiers.insert(std::make_pair("atomicXor", Identifier("int atomicXor(inout int mem, int data)\nuint atomicXor(inout uint mem, uint data)\nPerform an atomic logical exclusive OR operation to a variable")));
		}

		{
			langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("[ \\t]*#[ \\t]*[a-zA-Z_]+", PaletteIndex::Preprocessor));
			langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("L?\\\"(\\\\.|[^\\\"])*\\\"", PaletteIndex::String));
			langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("\\'\\\\?[^\\']\\'", PaletteIndex::CharLiteral));
			langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)([eE][+-]?[0-9]+)?[fF]?", PaletteIndex::Number));
			langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("[+-]?[0-9]+[Uu]?[lL]?[lL]?", PaletteIndex::Number));
			langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("0[0-7]+[Uu]?[lL]?[lL]?", PaletteIndex::Number));
			langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("0[xX][0-9a-fA-F]+[uU]?[lL]?[lL]?", PaletteIndex::Number));
			langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("[a-zA-Z_][a-zA-Z0-9_]*", PaletteIndex::Identifier));
			langDef.mTokenRegexStrings.push_back(std::make_pair<std::string, PaletteIndex>("[\\[\\]\\{\\}\\!\\%\\^\\&\\*\\(\\)\\-\\+\\=\\~\\|\\<\\>\\?\\/\\;\\,\\.]", PaletteIndex::Punctuation));
		}

		langDef.mCommentStart = "/*";
		langDef.mCommentEnd = "*/";
		langDef.mSingleLineComment = "//";

		langDef.mCaseSensitive = true;
		langDef.mAutoIndentation = true;

		langDef.mName = "GLSL";

		initialized = true;
	}

	return langDef;
}
