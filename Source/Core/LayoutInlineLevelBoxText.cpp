/*
 * This source file is part of RmlUi, the HTML/CSS Interface Middleware
 *
 * For the latest information, see http://github.com/mikke89/RmlUi
 *
 * Copyright (c) 2008-2010 CodePoint Ltd, Shift Technology Ltd
 * Copyright (c) 2019 The RmlUi Team, and contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "LayoutInlineLevelBoxText.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/Core.h"
#include "../../Include/RmlUi/Core/ElementText.h"
#include "../../Include/RmlUi/Core/ElementUtilities.h"
#include "../../Include/RmlUi/Core/FontEngineInterface.h"
#include "../../Include/RmlUi/Core/Log.h"
#include "../../Include/RmlUi/Core/Profiling.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "ComputeProperty.h"
#include "LayoutEngine.h"
#include "LayoutLineBox.h"

namespace Rml {

FragmentResult InlineLevelBox_Text::CreateFragment(InlineLayoutMode mode, float available_width, float right_spacing_width, bool first_box,
	LayoutOverflowHandle in_overflow_handle)
{
	ElementText* text_element = GetTextElement();

	// TODO: Allow empty if we have floats too, then we can wrap down. (But never if we cannot wrap?). Force it if we are the first content box of
	// this line. That is, purely opened inline boxes that we are contained within should not count as a first box (we are then still the first box).
	const bool allow_empty = (mode == InlineLayoutMode::WrapAny);
	const bool decode_escape_characters = true;

	String line_contents;
	int line_begin = in_overflow_handle;
	int line_length = 0;
	float line_width = 0.f;
	bool overflow = !text_element->GenerateLine(line_contents, line_length, line_width, line_begin, available_width, right_spacing_width, first_box,
		decode_escape_characters, allow_empty);

	if (overflow && line_contents.empty())
		// We couldn't fit anything on this line.
		return {};

	LayoutOverflowHandle out_overflow_handle = {};
	if (overflow)
		out_overflow_handle = line_begin + line_length;

	const bool principal_fragment = (line_begin == 0);

	return FragmentResult(FragmentType::TextRun, principal_fragment, line_width, 0.f, 0.f, out_overflow_handle, std::move(line_contents));
}

void InlineLevelBox_Text::Submit(FragmentBox fragment_box, String text)
{
	ElementText* text_element = GetTextElement();
	Vector2f line_offset;
	if (fragment_box.principal_box)
	{
		text_element->SetOffset(fragment_box.position, fragment_box.offset_parent);
		text_element->ClearLines();
	}
	else
	{
		// TODO: Will be wrong in case of relative positioning. (we really just want to subtract the value submitted to SetOffset in Submit() above).
		const Vector2f element_offset = text_element->GetRelativeOffset(Box::BORDER);
		line_offset = fragment_box.position - element_offset;
	}

	text_element->AddLine(line_offset, std::move(text));

	// TODO Use offset calculation from base function.
	// TODO Maybe we want to size it?
}

String InlineLevelBox_Text::DebugDumpNameValue() const
{
	return "InlineLevelBox_Text";
}

ElementText* InlineLevelBox_Text::GetTextElement()
{
	RMLUI_ASSERT(rmlui_dynamic_cast<ElementText*>(GetElement()));

	return static_cast<ElementText*>(GetElement());
}

} // namespace Rml
