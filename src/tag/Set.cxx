/*
 * Copyright 2003-2017 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "Set.hxx"
#include "TagBuilder.hxx"
#include "Settings.hxx"

#include <assert.h>

/**
 * Copy all tag items of the specified type.
 */
static bool
CopyTagItem(TagBuilder &dest, TagType dest_type,
	    const Tag &src, TagType src_type)
{
	bool found = false;

	for (const auto &item : src) {
		if (item.type == src_type) {
			dest.AddItem(dest_type, item.value);
			found = true;
		}
	}

	return found;
}

/**
 * Copy all tag items of the specified type.  Fall back to "Artist" if
 * there is no "AlbumArtist".
 */
static void
CopyTagItem(TagBuilder &dest, const Tag &src, TagType type)
{
	if (!CopyTagItem(dest, type, src, type) &&
	    type == TAG_ALBUM_ARTIST)
		CopyTagItem(dest, type, src, TAG_ARTIST);
}

/**
 * Copy all tag items of the types in the mask.
 */
static void
CopyTagMask(TagBuilder &dest, const Tag &src, tag_mask_t mask)
{
	for (unsigned i = 0; i < TAG_NUM_OF_ITEM_TYPES; ++i)
		if ((mask & (tag_mask_t(1) << i)) != 0)
			CopyTagItem(dest, src, TagType(i));
}

void
TagSet::InsertUnique(const Tag &src, TagType type, const char *value,
		     tag_mask_t group_mask) noexcept
{
	TagBuilder builder;
	if (value == nullptr)
		builder.AddEmptyItem(type);
	else
		builder.AddItem(type, value);
	CopyTagMask(builder, src, group_mask);
#if CLANG_OR_GCC_VERSION(4,8)
	emplace(builder.Commit());
#else
	insert(builder.Commit());
#endif
}

bool
TagSet::CheckUnique(TagType dest_type,
		    const Tag &tag, TagType src_type,
		    tag_mask_t group_mask) noexcept
{
	bool found = false;

	for (const auto &item : tag) {
		if (item.type == src_type) {
			InsertUnique(tag, dest_type, item.value, group_mask);
			found = true;
		}
	}

	return found;
}

void
TagSet::InsertUnique(const Tag &tag,
		     TagType type, tag_mask_t group_mask) noexcept
{
	static_assert(sizeof(group_mask) * 8 >= TAG_NUM_OF_ITEM_TYPES,
		      "Mask is too small");

	assert((group_mask & (tag_mask_t(1) << unsigned(type))) == 0);

	if (!CheckUnique(type, tag, type, group_mask) &&
	    (type != TAG_ALBUM_ARTIST ||
	     !IsTagEnabled(TAG_ALBUM_ARTIST) ||
	     /* fall back to "Artist" if no "AlbumArtist" was found */
	     !CheckUnique(type, tag, TAG_ARTIST, group_mask)))
		InsertUnique(tag, type, nullptr, group_mask);
}
