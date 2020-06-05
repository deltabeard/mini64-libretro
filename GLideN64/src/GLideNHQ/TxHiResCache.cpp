/*
 * Texture Filtering
 * Version:  1.0
 *
 * Copyright (C) 2007  Hiroshi Morii   All Rights Reserved.
 * Email koolsmoky(at)users.sourceforge.net
 * Web   http://www.3dfxzone.it/koolsmoky
 *
 * this is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * this is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Make; see the file COPYING.  If not, write to
 * the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* 2007 Gonetz <gonetz(at)ngs.ru>
 * Added callback to display hires texture info. */

#ifdef __MSC__
#pragma warning(disable: 4786)
#endif

/* use power of 2 texture size
 * (0:disable, 1:enable, 2:3dfx) */
#define POW2_TEXTURES 0

/* use aggressive format assumption for quantization
 * (0:disable, 1:enable, 2:extreme) */
#define AGGRESSIVE_QUANTIZATION 1

#include "TxHiResCache.h"
#include "TxDbg.h"
#include <zlib.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HIRES_DUMP_ENABLED (FILE_HIRESTEXCACHE|DUMP_HIRESTEXCACHE)

TxHiResCache::~TxHiResCache()
{
}

TxHiResCache::TxHiResCache(int maxwidth,
		int maxheight,
		int maxbpp,
		int options,
		const wchar_t *cachePath,
		const wchar_t *texPackPath,
		const wchar_t *ident)
	: TxCache((options & ~(GZ_TEXCACHE | FILE_TEXCACHE)), 0, cachePath, ident)
	, _maxwidth(maxwidth)
	, _maxheight(maxheight)
	, _maxbpp(maxbpp)
	, _abortLoad(false)
	, _cacheDumped(false)
	, _txImage(new TxImage())
	, _txQuantize(new TxQuantize())
	  , _txReSample(new TxReSample())
{

	if (texPackPath)
		_texPackPath.assign(texPackPath);

	if (_cachePath.empty() || _ident.empty()) {
		setOptions(getOptions() & ~HIRES_DUMP_ENABLED);
		return;
	}

	/* read in hires texture cache */
	if (getOptions() & HIRES_DUMP_ENABLED) {
		/* find it on disk */
		_cacheDumped = TxCache::load(!_HiResTexPackPathExists());
	}

	/* read in hires textures */
	if (!_cacheDumped) {
		if (TxHiResCache::load(0) && (getOptions() & HIRES_DUMP_ENABLED) != 0)
			_cacheDumped = TxCache::save();
	}
}

void TxHiResCache::dump()
{
	if ((getOptions() & HIRES_DUMP_ENABLED) && !_cacheDumped && !_abortLoad && !empty()) {
	  /* dump cache to disk */
	  _cacheDumped = TxCache::save();
	}
}

tx_wstring TxHiResCache::_getFileName() const
{
	tx_wstring filename = _ident + wst("_HIRESTEXTURES.");
	filename += ((getOptions() & FILE_HIRESTEXCACHE) == 0) ? TEXCACHE_EXT : TEXSTREAM_EXT;
	removeColon(filename);
	return filename;
}

int TxHiResCache::_getConfig() const
{
	return getOptions() &
		(HIRESTEXTURES_MASK |
		TILE_HIRESTEX |
		FORCE16BPP_HIRESTEX |
		GZ_HIRESTEXCACHE |
		FILE_HIRESTEXCACHE |
		LET_TEXARTISTS_FLY);
}

boolean TxHiResCache::_HiResTexPackPathExists() const
{
	return false;
}

bool TxHiResCache::load(boolean replace) /* 0 : reload, 1 : replace partial */
{
	return false;
}

TxHiResCache::LoadResult TxHiResCache::loadHiResTextures(const wchar_t * dir_path, boolean replace)
{
	return resNotFound;
}
