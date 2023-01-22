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

#include "LayoutLineBox.h"
#include "../../Include/RmlUi/Core/ComputedValues.h"
#include "../../Include/RmlUi/Core/ElementText.h"
#include "../../Include/RmlUi/Core/ElementUtilities.h"
#include "../../Include/RmlUi/Core/Profiling.h"
#include "../../Include/RmlUi/Core/Property.h"
#include "LayoutBlockBox.h"
#include "LayoutEngine.h"
#include "LayoutInlineBox.h"
#include "LayoutInlineContainer.h"
#include "LayoutInlineLevelBoxText.h"
#include <algorithm>
#include <numeric>

namespace Rml {

LayoutLineBox::~LayoutLineBox() {}

bool LayoutLineBox::AddBox(InlineLevelBox* box, InlineLayoutMode layout_mode, float line_width, LayoutOverflowHandle& inout_overflow_handle)
{
	RMLUI_ASSERT(!is_closed);

	// TODO: The spacing this element must leave on the right of the line, to account not only for its margins and padding,
	// but also for its parents which will close immediately after it.
	// (Right edge width of all open fragments)
	// TODO: We don't necessarily need to consider all the open boxes if there is content coming after this box.
	const bool first_box = !HasContent();

	float open_spacing_right = 0.f;
	for (int fragment_index : open_fragments)
		open_spacing_right += fragments[fragment_index].box->GetSpacingRight();

	const float box_placement_cursor = box_cursor + open_spacing_left;

	// TODO: Maybe always pass the actual available width, and let the createfragment functions handle the mode correctly.
	float available_width = FLT_MAX;
	if (layout_mode != InlineLayoutMode::Nowrap)
	{
		available_width = Math::RoundUpFloat(line_width - box_placement_cursor);
		if (available_width < 0.f)
		{
			if (layout_mode == InlineLayoutMode::WrapAny)
				return true;
			else
				available_width = 0.f;
		}
	}

	FragmentResult fragment = box->CreateFragment(layout_mode, available_width, open_spacing_right, first_box, inout_overflow_handle);
	inout_overflow_handle = {};

	if (fragment.type == FragmentType::Invalid)
	{
		// Could not place fragment on this line, try again on a new line.
		RMLUI_ASSERT(layout_mode == InlineLayoutMode::WrapAny);
		return true;
	}

	const FragmentIndex fragment_index = (int)fragments.size();

	fragments.push_back({});
	Fragment& new_fragment = fragments.back();
	new_fragment.box = box;
	new_fragment.fragment_handle = fragment.fragment_handle;
	new_fragment.type = fragment.type;
	new_fragment.vertical_align = box->GetVerticalAlign();
	new_fragment.position.x = box_placement_cursor;
	new_fragment.layout_width = fragment.layout_width;
	new_fragment.parent_fragment = GetOpenParent();
	new_fragment.aligned_subtree_root = DetermineAlignedSubtreeRoot(fragment_index);

	bool continue_on_new_line = false;

	switch (fragment.type)
	{
	case FragmentType::InlineBox:
	{
		// Opens up an inline box.
		RMLUI_ASSERT(fragment.layout_width < 0.f);
		RMLUI_ASSERT(rmlui_dynamic_cast<InlineBox*>(box));

		open_fragments.push_back(fragment_index);
		open_spacing_left += box->GetSpacingLeft();
	}
	break;
	case FragmentType::SizedBox:
	case FragmentType::TextRun:
	{
		// Fixed-size fragment.
		RMLUI_ASSERT(fragment.layout_width >= 0.f);

		box_cursor = box_placement_cursor + fragment.layout_width;
		open_spacing_left = 0.f;

		if (fragment.overflow_handle)
		{
			continue_on_new_line = true;
			inout_overflow_handle = fragment.overflow_handle;
		}

		// Mark open fragments as having content so we later know whether we should split or move them in case of overflow.
		for (FragmentIndex index : open_fragments)
			fragments[index].has_content = true;
	}
	break;
	case FragmentType::Invalid:
		RMLUI_ERROR; // Handled above;
		break;
	}

	return continue_on_new_line;
}

void LayoutLineBox::VerticallyAlignSubtree(const int subtree_root_index, const int children_end_index, float& max_ascent, float& max_descent)
{
	const int children_begin_index = subtree_root_index + 1;

	// Iterate all descendant fragments which belong to our aligned subtree.
	for (int i = children_begin_index; i < children_end_index; i++)
	{
		Fragment& fragment = fragments[i];
		if (fragment.aligned_subtree_root != subtree_root_index)
			continue;

		// Position the baseline of fragments relative to their subtree root.
		const float parent_absolute_baseline = (fragment.parent_fragment < 0 ? 0.f : fragments[fragment.parent_fragment].baseline_offset);
		fragment.baseline_offset = parent_absolute_baseline + fragment.box->GetVerticalOffsetFromParent();

		// Expand this aligned subtree's height based on the height contributions of its descendants.
		if (fragment.type != FragmentType::TextRun)
		{
			max_ascent = Math::Max(max_ascent, fragment.box->GetHeightAboveBaseline() - fragment.baseline_offset);
			max_descent = Math::Max(max_descent, fragment.box->GetDepthBelowBaseline() + fragment.baseline_offset);
		}
	}
}

UniquePtr<LayoutLineBox> LayoutLineBox::Close(const InlineBoxRoot* root_inline_box, Element* offset_parent, Vector2f line_position,
	Style::TextAlign text_align, float& out_height_of_line)
{
	RMLUI_ASSERT(!is_closed);

	UniquePtr<LayoutLineBox> new_line_box = SplitLine();

	RMLUI_ASSERT(open_fragments.empty());

	// Vertical alignment and sizing.
	//
	// Aligned subtree (CSS definition): "The aligned subtree of an inline element contains that element and the aligned
	// subtrees of all children inline elements whose computed vertical-align value is not 'top' or 'bottom'."
	//
	// We have already determined each box's offset relative to its parent baseline, and its layout size above and below
	// its baseline. Now, for each aligned subtree, place all fragments belonging to that subtree relative to the
	// subtree root baseline. Simultaneously, consider each fragment and keep track of the maximum height above root
	// baseline (max_ascent) and maximum depth below root baseline (max_descent). Their sum is the height of that
	// aligned subtree.
	//
	// Next, treat the root inline box like just another aligned subtree. Then the line box height is first determined
	// by the height of that subtree. Other aligned subtrees might push out that size to make them fit. After the line
	// box size is determined, position each aligned subtree according to its vertical-align, and then position each
	// fragment relative to the aligned subtree they belong to.
	{
		using Style::VerticalAlign;

		float max_ascent = root_inline_box->GetHeightAboveBaseline();
		float max_descent = root_inline_box->GetDepthBelowBaseline();

		{
			const int subtree_root_index = -1;
			const int children_end_index = (int)fragments.size();
			VerticallyAlignSubtree(subtree_root_index, children_end_index, max_ascent, max_descent);
		}

		// Find all the aligned subtrees, and vertically align each of them independently.
		for (int i = 0; i < (int)fragments.size(); i++)
		{
			Fragment& fragment = fragments[i];
			if (IsAlignedSubtreeRoot(fragment))
			{
				fragment.max_ascent = fragment.box->GetHeightAboveBaseline();
				fragment.max_descent = fragment.box->GetDepthBelowBaseline();

				if (fragment.type == FragmentType::InlineBox)
				{
					const int subtree_root_index = i;
					VerticallyAlignSubtree(subtree_root_index, fragment.children_end_index, fragment.max_ascent, fragment.max_descent);
				}

				// Increase the line box size to fit all line-relative aligned fragments.
				switch (fragment.vertical_align)
				{
				case VerticalAlign::Top: max_descent = Math::Max(max_descent, fragment.max_ascent + fragment.max_descent - max_ascent); break;
				case VerticalAlign::Bottom: max_ascent = Math::Max(max_ascent, fragment.max_ascent + fragment.max_descent - max_descent); break;
				default: RMLUI_ERROR; break;
				}
			}
		}

		// Size the line.
		out_height_of_line = max_ascent + max_descent;
		total_height_above_baseline = max_ascent;

		// Now that the line is sized we can set the vertical position of the fragments.
		for (Fragment& fragment : fragments)
		{
			switch (fragment.vertical_align)
			{
			case VerticalAlign::Top: fragment.position.y = fragment.max_ascent; break;
			case VerticalAlign::Bottom: fragment.position.y = out_height_of_line - fragment.max_descent; break;
			default:
			{
				RMLUI_ASSERT(!IsAlignedSubtreeRoot(fragment));
				const float aligned_subtree_baseline =
					(fragment.aligned_subtree_root < 0 ? max_ascent : fragments[fragment.aligned_subtree_root].position.y);
				fragment.position.y = aligned_subtree_baseline + fragment.baseline_offset;
			}
			break;
			}
		}
	}

	// Horizontal alignment using available space on our line.
	if (box_cursor < line_width)
	{
		switch (text_align)
		{
		case Style::TextAlign::Center: offset_horizontal_alignment = (line_width - box_cursor) * 0.5f; break;
		case Style::TextAlign::Right: offset_horizontal_alignment = (line_width - box_cursor); break;
		case Style::TextAlign::Left:    // Already left-aligned.
		case Style::TextAlign::Justify: // Justification occurs at the text level.
			break;
		}
	}

	// Position and size all inline-level boxes, place geometry boxes.
	for (const auto& fragment : fragments)
	{
		// Skip inline-boxes which have not been closed (moved down to next line).
		if (fragment.type == FragmentType::InlineBox && fragment.children_end_index == 0)
			continue;

		RMLUI_ASSERT(fragment.layout_width >= 0.f);

		FragmentBox fragment_box = {
			offset_parent,
			fragment.fragment_handle,
			line_position + fragment.position + Vector2f(offset_horizontal_alignment, 0.f),
			fragment.layout_width,
			fragment.split_left,
			fragment.split_right,
		};
		fragment.box->Submit(fragment_box);
	}

	is_closed = true;

	return new_line_box;
}

LayoutLineBox::Fragment& LayoutLineBox::CloseFragment(int open_fragment_index, float right_inner_edge_position)
{
	Fragment& open_fragment = fragments[open_fragment_index];
	RMLUI_ASSERT(open_fragment.type == FragmentType::InlineBox);

	open_fragment.children_end_index = (int)fragments.size();
	open_fragment.layout_width =
		Math::Max(right_inner_edge_position - open_fragment.position.x - (open_fragment.split_left ? 0.f : open_fragment.box->GetSpacingLeft()), 0.f);

	return open_fragment;
}

UniquePtr<LayoutLineBox> LayoutLineBox::SplitLine()
{
	if (open_fragments.empty())
		return nullptr;

	// Make a new line with the open fragments.
	auto new_line = MakeUnique<LayoutLineBox>();
	new_line->fragments.reserve(open_fragments.size());

	// Copy all open fragments to the next line. Fragments that had any content placed on the previous line is split,
	// otherwise the whole fragment is moved down.
	for (const int fragment_index : open_fragments)
	{
		const FragmentIndex new_index = (FragmentIndex)new_line->fragments.size();

		new_line->fragments.push_back(Fragment{fragments[fragment_index]});

		Fragment& fragment = new_line->fragments.back();
		fragment.position.x = new_line->box_cursor;
		fragment.parent_fragment = new_index - 1;
		fragment.aligned_subtree_root = new_line->DetermineAlignedSubtreeRoot(new_index);

		if (fragment.has_content)
		{
			fragment.split_left = true;
			fragment.has_content = false;
		}
		else
		{
			new_line->open_spacing_left += fragment.box->GetSpacingLeft();
		}
	}

	// Close any open fragments that have content, splitting their right side.
	for (auto it = open_fragments.rbegin(); it != open_fragments.rend(); ++it)
	{
		const int fragment_index = *it;
		if (fragments[fragment_index].has_content)
		{
			Fragment& closed_fragment = CloseFragment(fragment_index, box_cursor);
			closed_fragment.split_right = true;
		}
	}

	// Steal the open fragment vector memory, as it is no longer needed here.
	new_line->open_fragments = std::move(open_fragments);
	std::iota(new_line->open_fragments.begin(), new_line->open_fragments.end(), 0);
	open_fragments.clear();

#ifdef RMLUI_DEBUG
	// Verify integrity of the fragment tree after split.
	for (int i = 0; i < (int)new_line->fragments.size(); i++)
	{
		const Fragment& fragment = new_line->fragments[i];
		RMLUI_ASSERT(fragment.type == FragmentType::InlineBox);
		RMLUI_ASSERT(fragment.parent_fragment < i);
		RMLUI_ASSERT(fragment.parent_fragment == -1 || new_line->fragments[fragment.parent_fragment].type == FragmentType::InlineBox);
		RMLUI_ASSERT(fragment.aligned_subtree_root == -1 || new_line->IsAlignedSubtreeRoot(new_line->fragments[fragment.aligned_subtree_root]));
		RMLUI_ASSERT(fragment.children_end_index == 0);
	}
	RMLUI_ASSERT(new_line->open_fragments.size() == new_line->fragments.size());
#endif

	return new_line;
}

void LayoutLineBox::CloseInlineBox(InlineBox* inline_box)
{
	if (open_fragments.empty() || fragments[open_fragments.back()].box != inline_box)
	{
		RMLUI_ERRORMSG("Inline box open/close mismatch.");
		return;
	}

	box_cursor += open_spacing_left;
	open_spacing_left = 0.f;

	const Fragment& closed_fragment = CloseFragment(open_fragments.back(), box_cursor);
	box_cursor += closed_fragment.box->GetSpacingRight();

	open_fragments.pop_back();
}

InlineBox* LayoutLineBox::GetOpenInlineBox()
{
	if (open_fragments.empty())
		return nullptr;

	return static_cast<InlineBox*>(fragments[open_fragments.back()].box);
}

void LayoutLineBox::SetLineBox(Vector2f _line_position, float _line_width)
{
	line_position = _line_position;
	line_width = _line_width;
}

float LayoutLineBox::GetExtentRight() const
{
	RMLUI_ASSERT(is_closed);
	return box_cursor + offset_horizontal_alignment;
}

float LayoutLineBox::GetBaseline() const
{
	RMLUI_ASSERT(is_closed);
	return total_height_above_baseline;
}

String LayoutLineBox::DebugDumpTree(int depth) const
{
	const String value =
		String(depth * 2, ' ') + "LayoutLineBox (" + ToString(fragments.size()) + " fragment" + (fragments.size() == 1 ? "" : "s") + ")\n";

	return value;
}

void* LayoutLineBox::operator new(size_t size)
{
	return LayoutEngine::AllocateLayoutChunk(size);
}

void LayoutLineBox::operator delete(void* chunk, size_t size)
{
	LayoutEngine::DeallocateLayoutChunk(chunk, size);
}

} // namespace Rml
