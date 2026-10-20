// TClient: bulk compression of embedded map resources, triggered from the
// multi-file selection popup in the images/sounds lists.
//
// Images: zeroes every pixel no layer can sample (twmap-style per-pixel quad
// usage rasterization plus the 16x16 tile grid of tiles layers), then dilates
// the remaining transparent pixels with the engine dilate. The resulting
// pixel data is identical in size but compresses much better when the map is
// written (datafile deflate), without changing what the map looks like.
//
// Sounds: embedded map sounds are plain ogg-opus files. They are decoded with
// opusfile and re-encoded with the bundled libopus encoder at roughly half
// the original bitrate into a fresh ogg container, which every client can
// play just like the original.

#include "bulk_compress.h"

#include <base/dbg.h>
#include <base/log.h>
#include <base/math.h>
#include <base/mem.h>
#include <base/str.h>

#include <engine/gfx/image_manipulation.h>
#include <engine/engine.h>
#include <engine/graphics.h>
#include <engine/sound.h>

#include <ogg/ogg.h>
#include <opus/opus.h>
#include <opusfile.h>
#include <zlib.h>

#include <game/editor/editor.h>
#include <game/editor/editor_actions.h>
#include <game/editor/mapitems/image.h>
#include <game/editor/mapitems/layer_group.h>
#include <game/editor/mapitems/layer_quads.h>
#include <game/editor/mapitems/layer_tiles.h>
#include <game/editor/mapitems/map.h>
#include <game/editor/mapitems/sound.h>

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <vector>

// ------------------------------ images -------------------------------------

void RefreshImageTexture(CEditorImage *pImage)
{
	pImage->Graphics()->UnloadTexture(&pImage->m_Texture);
	int TextureLoadFlag = pImage->Graphics()->TextureLoadFlags();
	if(pImage->m_Width % 16 != 0 || pImage->m_Height % 16 != 0)
		TextureLoadFlag = 0;
	pImage->m_Texture = pImage->Graphics()->LoadTextureRaw(*pImage, TextureLoadFlag, pImage->m_aName);
}

void ApplyImageData(CEditorImage *pImage, const std::vector<uint8_t> &vData, size_t Width, size_t Height)
{
	// free the pixels and the texture without touching the automapper config
	// (CEditorImage::Free would also unload it and it is not reloaded lazily)
	pImage->Graphics()->UnloadTexture(&pImage->m_Texture);
	pImage->CImageInfo::Free();
	pImage->m_Width = Width;
	pImage->m_Height = Height;
	pImage->m_Format = CImageInfo::FORMAT_RGBA;
	pImage->Allocate();
	mem_copy(pImage->m_pData, vData.data(), vData.size());
	pImage->MarkDataModified();
	pImage->AnalyseTileFlags();
	RefreshImageTexture(pImage);
}

// Halves the image resolution with area averaging. The target dimensions are
// rounded down to multiples of 16 so tileset images keep their 16x16 tile
// grid and stay eligible for mipmapped textures. Returns false for images
// smaller than 512px on a side, where the quality loss would outweigh the
// gain.
static bool DownscaleImageHalf(const CEditorImage *pImage, std::vector<uint8_t> &rOut, size_t &rWidth, size_t &rHeight)
{
	const size_t Width = pImage->m_Width;
	const size_t Height = pImage->m_Height;
	if(Width < 512 || Height < 512)
		return false;

	const size_t NewWidth = std::max<size_t>(16, (Width / 2 / 16) * 16);
	const size_t NewHeight = std::max<size_t>(16, (Height / 2 / 16) * 16);
	rOut.assign(NewWidth * NewHeight * 4, 0);

	const double ScaleX = (double)Width / NewWidth;
	const double ScaleY = (double)Height / NewHeight;
	const uint8_t *pSrc = pImage->m_pData;
	for(size_t y = 0; y < NewHeight; ++y)
	{
		const double SrcY0 = y * ScaleY;
		const double SrcY1 = (y + 1) * ScaleY;
		for(size_t x = 0; x < NewWidth; ++x)
		{
			const double SrcX0 = x * ScaleX;
			const double SrcX1 = (x + 1) * ScaleX;

			double aAcc[4] = {0.0, 0.0, 0.0, 0.0};
			double Area = 0.0;
			const int PxEnd = std::min((int)std::ceil(SrcX1), (int)Width);
			const int PyEnd = std::min((int)std::ceil(SrcY1), (int)Height);
			for(int py = (int)SrcY0; py < PyEnd; ++py)
			{
				const double fy = std::min(SrcY1, py + 1.0) - std::max(SrcY0, (double)py);
				if(fy <= 0.0)
					continue;
				for(int px = (int)SrcX0; px < PxEnd; ++px)
				{
					const double fx = std::min(SrcX1, px + 1.0) - std::max(SrcX0, (double)px);
					if(fx <= 0.0)
						continue;
					const double Weight = fx * fy;
					const uint8_t *pPixel = pSrc + ((size_t)py * Width + px) * 4;
					for(int c = 0; c < 4; ++c)
						aAcc[c] += pPixel[c] * Weight;
					Area += Weight;
				}
			}

			uint8_t *pDst = rOut.data() + (y * NewWidth + x) * 4;
			for(int c = 0; c < 4; ++c)
				pDst[c] = Area > 0.0 ? (uint8_t)std::lround(aAcc[c] / Area) : 0;
		}
	}

	rWidth = NewWidth;
	rHeight = NewHeight;
	return true;
}

// signed side of point p relative to the line a-b (positive = left of a->b)
static int64_t TriSide(int64_t px, int64_t py, int64_t ax, int64_t ay, int64_t bx, int64_t by)
{
	return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static int64_t WrapCoord(int64_t Value, int64_t Dim)
{
	return ((Value % Dim) + Dim) % Dim;
}

// Marks every image pixel a quad can sample by rasterizing its texture
// coordinates (22.10 fixed point -> texel space) with wrap-around, mirroring
// the approach of twmap's optimize_mapres.
static void MarkQuadTextureUsage(const CQuad &Quad, int Width, int Height, std::vector<bool> &vMask)
{
	int64_t aX[4];
	int64_t aY[4];
	for(int v = 0; v < 4; ++v)
	{
		aX[v] = (int64_t)Quad.m_aTexcoords[v].x * Width / 1024;
		aY[v] = (int64_t)Quad.m_aTexcoords[v].y * Height / 1024;
	}

	static constexpr int TRIS[2][3] = {{0, 1, 3}, {0, 2, 3}};
	for(const auto &Tri : TRIS)
	{
		int64_t ax = aX[Tri[0]], ay = aY[Tri[0]];
		int64_t bx = aX[Tri[1]], by = aY[Tri[1]];
		int64_t cx = aX[Tri[2]], cy = aY[Tri[2]];

		// normalize winding so "inside" is a consistent set of non-negative side tests
		if(TriSide(cx, cy, ax, ay, bx, by) < 0)
		{
			std::swap(bx, cx);
			std::swap(by, cy);
		}

		const int64_t MinX = std::min(std::min(ax, bx), cx);
		const int64_t MaxX = std::max(std::max(ax, bx), cx);
		const int64_t MinY = std::min(std::min(ay, by), cy);
		const int64_t MaxY = std::max(std::max(ay, by), cy);
		// texel coordinates far outside the image only occur for pathological
		// uv values; allow one full wrap beyond each edge
		const int64_t LoX = std::max(MinX, (int64_t)-Width);
		const int64_t HiX = std::min(MaxX, (int64_t)2 * Width);
		const int64_t LoY = std::max(MinY, (int64_t)-Height);
		const int64_t HiY = std::min(MaxY, (int64_t)2 * Height);

		for(int64_t py = LoY; py <= HiY; ++py)
		{
			for(int64_t px = LoX; px <= HiX; ++px)
			{
				if(TriSide(px, py, ax, ay, bx, by) >= 0 &&
					TriSide(px, py, bx, by, cx, cy) >= 0 &&
					TriSide(px, py, cx, cy, ax, ay) >= 0)
				{
					const size_t wx = (size_t)WrapCoord(px, Width);
					const size_t wy = (size_t)WrapCoord(py, Height);
					vMask[wy * Width + wx] = true;
				}
			}
		}
	}
}

// Expands the used-pixel mask by one pixel (4-neighborhood, wrapping at the
// image borders) so bilinear filtering near quad edges can never reach into
// the cleared area.
static void WidenMask(const std::vector<bool> &vSrc, std::vector<bool> &vDst, int Width, int Height)
{
	static constexpr int DIRS[4][2] = {{0, 1}, {1, 0}, {0, -1}, {-1, 0}};
	for(int y = 0; y < Height; ++y)
	{
		for(int x = 0; x < Width; ++x)
		{
			const size_t Index = (size_t)y * Width + x;
			if(vSrc[Index])
			{
				vDst[Index] = true;
				continue;
			}
			bool Any = false;
			for(const auto &Dir : DIRS)
			{
				const int nx = (x + Dir[0] + Width) % Width;
				const int ny = (y + Dir[1] + Height) % Height;
				if(vSrc[(size_t)ny * Width + nx])
				{
					Any = true;
					break;
				}
			}
			vDst[Index] = Any;
		}
	}
}

void CompressEditorImagePixels(CEditorMap *pMap, int ImageIndex, CEditorImage *pImage)
{
	const int Width = pImage->m_Width;
	const int Height = pImage->m_Height;
	dbg_assert(Width > 0 && Height > 0 && pImage->m_pData != nullptr, "image must have data");

	bool aTileUsed[256] = {false};
	bool TileActive = false;
	bool QuadActive = false;

	for(const std::shared_ptr<CLayerGroup> &pGroup : pMap->m_vpGroups)
	{
		for(const std::shared_ptr<CLayer> &pLayer : pGroup->m_vpLayers)
		{
			if(auto *pTiles = dynamic_cast<CLayerTiles *>(pLayer.get()))
			{
				if(pTiles->m_Image != ImageIndex || pTiles->m_pTiles == nullptr)
					continue;
				TileActive = true;
				const int NumTiles = pTiles->m_Width * pTiles->m_Height;
				for(int i = 0; i < NumTiles; ++i)
				{
					const int TileIndex = pTiles->m_pTiles[i].m_Index;
					if(TileIndex > 0)
						aTileUsed[(TileIndex / 16) * 16 + (TileIndex % 16)] = true;
				}
			}
			else if(auto *pQuads = dynamic_cast<CLayerQuads *>(pLayer.get()))
			{
				if(pQuads->m_Image != ImageIndex)
					continue;
				QuadActive = true;
			}
		}
	}

	uint8_t *pData = pImage->m_pData;
	if(!TileActive && !QuadActive)
	{
		// nothing references the image, clear it completely
		mem_zero(pData, (size_t)Width * Height * 4);
	}
	else
	{
		// per-pixel usage mask from the quads that sample this image
		std::vector<bool> vMask((size_t)Width * Height, false);
		std::vector<bool> vMaskTmp((size_t)Width * Height, false);
		if(QuadActive)
		{
			for(const std::shared_ptr<CLayerGroup> &pGroup : pMap->m_vpGroups)
			{
				for(const std::shared_ptr<CLayer> &pLayer : pGroup->m_vpLayers)
				{
					auto *pQuads = dynamic_cast<CLayerQuads *>(pLayer.get());
					if(pQuads == nullptr || pQuads->m_Image != ImageIndex)
						continue;
					for(const CQuad &Quad : pQuads->m_vQuads)
						MarkQuadTextureUsage(Quad, Width, Height, vMask);
				}
			}
			// three double passes, matching the margin twmap uses
			for(int i = 0; i < 3; ++i)
			{
				WidenMask(vMask, vMaskTmp, Width, Height);
				WidenMask(vMaskTmp, vMask, Width, Height);
			}
		}

		const int TileW = std::max(1, Width / 16);
		const int TileH = std::max(1, Height / 16);
		for(int y = 0; y < Height; ++y)
		{
			for(int x = 0; x < Width; ++x)
			{
				bool Used = false;
				if(QuadActive && vMask[(size_t)y * Width + x])
					Used = true;
				if(TileActive && aTileUsed[std::min(15, y / TileH) * 16 + std::min(15, x / TileW)])
					Used = true;
				if(!Used)
				{
					uint8_t *pPixel = pData + ((size_t)y * Width + x) * 4;
					mem_zero(pPixel, 4);
				}
			}
		}
	}

	// bleed colors of opaque pixels into the transparent ones so semi
	// transparent edges do not pick up the zeroed RGB, and the deflate stage
	// gets smooth data
	DilateImage(pData, Width, Height);
}

// ------------------------------ sounds -------------------------------------

void ApplySoundData(CEditorSound *pSound, const void *pData, unsigned DataSize)
{
	void *pCopy = malloc(DataSize);
	if(pCopy == nullptr)
		return;
	mem_copy(pCopy, pData, DataSize);
	pSound->Sound()->UnloadSample(pSound->m_SoundId);
	free(pSound->m_pData);
	pSound->m_pData = pCopy;
	pSound->m_DataSize = DataSize;
	pSound->m_SoundId = pSound->Sound()->LoadOpusFromMem(pSound->m_pData, pSound->m_DataSize, true, pSound->m_aName);
}

static void WriteLe16(uint8_t *pDest, uint16_t Value)
{
	pDest[0] = Value & 0xff;
	pDest[1] = Value >> 8;
}

static void WriteLe32(uint8_t *pDest, uint32_t Value)
{
	pDest[0] = Value & 0xff;
	pDest[1] = (Value >> 8) & 0xff;
	pDest[2] = (Value >> 16) & 0xff;
	pDest[3] = Value >> 24;
}

static void AppendOggPage(std::vector<uint8_t> &rOut, const ogg_page &Page)
{
	rOut.insert(rOut.end(), Page.header, Page.header + Page.header_len);
	rOut.insert(rOut.end(), Page.body, Page.body + Page.body_len);
}

bool CompressEditorSoundBytes(const void *pData, unsigned DataSize, std::vector<uint8_t> &rOut)
{
	rOut.clear();

	int Error = OPUS_OK;
	OggOpusFile *pOpusFile = op_open_memory((const unsigned char *)pData, DataSize, &Error);
	if(pOpusFile == nullptr)
		return false;

	const int Channels = op_channel_count(pOpusFile, -1);
	if(Channels < 1 || Channels > 2)
	{
		op_free(pOpusFile);
		return false;
	}

	// decode everything; opusfile always decodes at 48 kHz
	constexpr int FrameSize = 960; // 20 ms
	std::vector<opus_int16> vPcm;
	vPcm.reserve(48000 * Channels);
	opus_int16 aChunk[5760 * 2];
	for(;;)
	{
		const int Samples = op_read(pOpusFile, aChunk, (int)std::size(aChunk), nullptr);
		if(Samples < 0)
		{
			op_free(pOpusFile);
			return false;
		}
		if(Samples == 0)
			break;
		vPcm.insert(vPcm.end(), aChunk, aChunk + (size_t)Samples * Channels);
	}
	op_free(pOpusFile);

	const int64_t TotalSamples = (int64_t)(vPcm.size() / Channels);
	if(TotalSamples <= 0)
		return false;

	// only worth it if there is meaningful room to shrink; keep the target at
	// half the estimated original bitrate but at a level that stays decent
	// for game sounds
	const double OriginalBitrate = DataSize * 8.0 / (TotalSamples / 48000.0);
	const int FloorBitrate = Channels == 2 ? 36000 : 18000;
	if(OriginalBitrate <= FloorBitrate * 1.25)
		return false;
	const int TargetBitrate = std::clamp((int)(OriginalBitrate * 0.5), FloorBitrate, 128000);

	Error = OPUS_OK;
	OpusEncoder *pEncoder = opus_encoder_create(48000, Channels, OPUS_APPLICATION_AUDIO, &Error);
	if(pEncoder == nullptr || Error != OPUS_OK)
	{
		if(pEncoder != nullptr)
			opus_encoder_destroy(pEncoder);
		return false;
	}
	opus_encoder_ctl(pEncoder, OPUS_SET_BITRATE(TargetBitrate));
	// the codec delays the signal by its lookahead; advertise that as the
	// container pre-skip (RFC 7845) so decoded output stays sample-aligned
	opus_int32 PreSkip = 0;
	opus_encoder_ctl(pEncoder, OPUS_GET_LOOKAHEAD(&PreSkip));

	ogg_stream_state Stream;
	ogg_stream_init(&Stream, 0x54433133); // "TC13"

	uint8_t aHead[19];
	mem_copy(aHead, "OpusHead", 8);
	aHead[8] = 1; // version
	aHead[9] = Channels;
	WriteLe16(aHead + 10, (uint16_t)PreSkip);
	WriteLe32(aHead + 12, 48000); // input sample rate
	WriteLe16(aHead + 16, 0); // output gain
	aHead[18] = 0; // mapping family

	const char VENDOR[] = "TClient editor";
	std::vector<uint8_t> vTags(8 + 4 + (sizeof(VENDOR) - 1) + 4);
	mem_copy(vTags.data(), "OpusTags", 8);
	WriteLe32(vTags.data() + 8, sizeof(VENDOR) - 1);
	mem_copy(vTags.data() + 12, VENDOR, sizeof(VENDOR) - 1);
	WriteLe32(vTags.data() + 12 + (sizeof(VENDOR) - 1), 0); // no user comments

	ogg_packet Packet;
	auto PushPacket = [&](const uint8_t *pPacketData, int64_t Bytes, bool Bos, bool Eos, int64_t Granule, int64_t PacketNo) {
		Packet.packet = const_cast<unsigned char *>(pPacketData);
		Packet.bytes = Bytes;
		Packet.b_o_s = Bos ? 1 : 0;
		Packet.e_o_s = Eos ? 1 : 0;
		Packet.granulepos = Granule;
		Packet.packetno = PacketNo;
		ogg_stream_packetin(&Stream, &Packet);
	};
	auto FlushPages = [&](bool Force) {
		ogg_page Page;
		if(Force)
		{
			while(ogg_stream_flush(&Stream, &Page) != 0)
				AppendOggPage(rOut, Page);
		}
		else
		{
			while(ogg_stream_pageout(&Stream, &Page) != 0)
				AppendOggPage(rOut, Page);
		}
	};

	PushPacket(aHead, sizeof(aHead), true, false, 0, 0);
	FlushPages(true);
	PushPacket(vTags.data(), (int64_t)vTags.size(), false, false, 0, 1);
	FlushPages(true);

	unsigned char aEncoded[4000];
	std::vector<opus_int16> vFrame(FrameSize * Channels, 0);
	// intermediate granule positions count encoded samples only; the codec
	// pre-skip is added to the final granule (matching libopusenc); the tail
	// is padded with zeros so the delayed signal fits into the stream
	const int64_t EncodeSamples = TotalSamples + PreSkip;
	int64_t Granule = 0;
	int64_t PacketNo = 2;
	bool Failed = false;
	for(int64_t Offset = 0; Offset < EncodeSamples && !Failed; Offset += FrameSize)
	{
		const int64_t FrameSamples = std::min<int64_t>(FrameSize, EncodeSamples - Offset);
		const int64_t CopySamples = std::min<int64_t>(FrameSamples, std::max<int64_t>(0, TotalSamples - Offset));
		if(CopySamples > 0)
			std::copy(vPcm.begin() + Offset * Channels, vPcm.begin() + (Offset + CopySamples) * Channels, vFrame.begin());
		std::fill(vFrame.begin() + CopySamples * Channels, vFrame.end(), 0);
		const int Bytes = opus_encode(pEncoder, vFrame.data(), FrameSize, aEncoded, sizeof(aEncoded));
		if(Bytes < 0)
		{
			Failed = true;
			break;
		}
		Granule += FrameSamples;
		const bool Last = Offset + FrameSize >= EncodeSamples;
		PushPacket(aEncoded, Bytes, false, Last, Last ? PreSkip + TotalSamples : Granule, PacketNo++);
		FlushPages(false);
	}

	FlushPages(true);
	opus_encoder_destroy(pEncoder);
	ogg_stream_clear(&Stream);

	if(Failed || rOut.empty())
	{
		rOut.clear();
		return false;
	}

	// sanity check: the result must be decodable opus with the same length
	OggOpusFile *pCheck = op_open_memory(rOut.data(), rOut.size(), &Error);
	if(pCheck == nullptr)
	{
		rOut.clear();
		return false;
	}
	const int64_t CheckSamples = op_pcm_total(pCheck, -1);
	op_free(pCheck);
	if(CheckSamples != TotalSamples)
	{
		rOut.clear();
		return false;
	}

	return true;
}

// --------------------------- editor entry points ----------------------------

void CEditor::CompressImages(const std::vector<int> &vIndices)
{
	std::vector<int> vCompress;
	for(int Index : vIndices)
	{
		if(Index < 0 || Index >= (int)Map()->m_vpImages.size())
			continue;
		std::shared_ptr<CEditorImage> pImg = Map()->m_vpImages[Index];
		if(pImg->m_External || pImg->m_pData == nullptr || pImg->m_Width <= 0 || pImg->m_Height <= 0)
			continue;
		vCompress.push_back(Index);
	}
	if(vCompress.empty())
	{
		ShowFileDialogError("Compressing is not possible: the selection contains no embedded images with data.");
		return;
	}

	std::sort(vCompress.begin(), vCompress.end());

	auto pAction = std::make_unique<CEditorActionCompressImages>(Map());
	int Compressed = 0;
	int Skipped = 0;
	for(int Index : vCompress)
	{
		std::shared_ptr<CEditorImage> pImg = Map()->m_vpImages[Index];
		const size_t DataSize = pImg->DataSize();
		const size_t OldWidth = pImg->m_Width;
		const size_t OldHeight = pImg->m_Height;
		std::vector<uint8_t> vOld((uint8_t *)pImg->m_pData, (uint8_t *)pImg->m_pData + DataSize);

		CompressEditorImagePixels(Map(), Index, pImg.get());
		if(std::equal(vOld.begin(), vOld.end(), (uint8_t *)pImg->m_pData))
		{
			// every pixel is in use, so nothing can be cleared: halve the
			// resolution instead, which keeps the look recognizable while
			// cutting the data to roughly a quarter
			std::vector<uint8_t> vDown;
			size_t DownWidth = 0;
			size_t DownHeight = 0;
			if(!DownscaleImageHalf(pImg.get(), vDown, DownWidth, DownHeight))
			{
				Skipped++;
				continue;
			}
			log_info("editor/compress", "halved image %d '%s': %" PRIzu "x%" PRIzu " -> %" PRIzu "x%" PRIzu,
				Index, pImg->m_aName, OldWidth, OldHeight, DownWidth, DownHeight);
			ApplyImageData(pImg.get(), vDown, DownWidth, DownHeight);
			pAction->AddImage(Index, std::move(vOld), OldWidth, OldHeight);
			Compressed++;
			continue;
		}

		RefreshImageTexture(pImg.get());
		pImg->MarkDataModified();
		pAction->AddImage(Index, std::move(vOld), OldWidth, OldHeight);
		Compressed++;
	}

	if(Compressed > 0)
	{
		Map()->m_EditorHistory.RecordAction(std::move(pAction));
		Map()->OnModify();
		log_info("editor/compress", "compressed %d images", Compressed);
		if(Skipped > 0)
			ShowFileDialogError("Compressed %d of %d images. The remaining ones are fully in use by the map and too small to shrink further.", Compressed, Compressed + Skipped);
	}
	else
	{
		ShowFileDialogError("Compressing had no effect: the selected images are fully in use by the map and too small to shrink further.");
	}
}

void CEditor::CompressSounds(const std::vector<int> &vIndices)
{
	std::vector<int> vCompress;
	for(int Index : vIndices)
	{
		if(Index < 0 || Index >= (int)Map()->m_vpSounds.size())
			continue;
		if(Map()->m_vpSounds[Index]->m_pData == nullptr || Map()->m_vpSounds[Index]->m_DataSize == 0)
			continue;
		vCompress.push_back(Index);
	}
	if(vCompress.empty())
	{
		ShowFileDialogError("Compressing is not possible: the selection contains no sounds with data.");
		return;
	}

	std::sort(vCompress.begin(), vCompress.end());

	auto pAction = std::make_unique<CEditorActionCompressSounds>(Map());
	int Compressed = 0;
	int Skipped = 0;
	for(int Index : vCompress)
	{
		std::shared_ptr<CEditorSound> pSound = Map()->m_vpSounds[Index];
		std::vector<uint8_t> vNew;
		if(!CompressEditorSoundBytes(pSound->m_pData, pSound->m_DataSize, vNew) || vNew.size() >= pSound->m_DataSize)
		{
			Skipped++;
			continue;
		}

		log_info("editor/compress", "compressed sound %d '%s': %u -> %llu bytes",
			Index, pSound->m_aName, pSound->m_DataSize, (unsigned long long)vNew.size());
		pAction->AddSound(Index, std::move(vNew));
		Compressed++;
	}

	if(Compressed > 0)
	{
		Map()->m_EditorHistory.RecordAction(std::move(pAction));
		Map()->OnModify();
		if(Skipped > 0)
			ShowFileDialogError("Compressed %d of %d sounds. The remaining ones are either not opus sounds or too small to shrink further.", Compressed, Compressed + Skipped);
	}
	else
	{
		ShowFileDialogError("Compressing had no effect: none of the selected sounds is a large enough opus sound to shrink further.");
	}
}

// --------------------------- size accounting -------------------------------

CEditorImageSizeJob::CEditorImageSizeJob(CEditorMap *pMap, const CEditorImage *pImage, uint64_t DataVersion, const void *pData, size_t DataSize) :
	m_pMap(pMap),
	m_pImage(pImage),
	m_DataVersion(DataVersion),
	m_vData((const uint8_t *)pData, (const uint8_t *)pData + DataSize)
{
}

void CEditorImageSizeJob::Run()
{
	uLongf CompressedSize = compressBound((uLong)m_vData.size());
	std::vector<uint8_t> vScratch(CompressedSize);
	if(compress2(vScratch.data(), &CompressedSize, m_vData.data(), m_vData.size(), Z_BEST_COMPRESSION) == Z_OK)
		m_CompressedSize = (size_t)CompressedSize;
	else
		m_CompressedSize = m_vData.size();
	m_Done.store(true);
}

static bool ImageEligibleForAccounting(const CEditorImage *pImage)
{
	return !pImage->m_External && pImage->m_pData != nullptr && pImage->m_Width > 0 && pImage->m_Height > 0;
}

void CEditor::UpdateImageSizeAccounting()
{
	// collect finished background jobs; nothing here ever blocks the frame
	for(auto It = m_vImageSizeJobs.begin(); It != m_vImageSizeJobs.end();)
	{
		std::shared_ptr<CEditorImageSizeJob> pJob = *It;
		if(!pJob->Done())
		{
			++It;
			continue;
		}

		// commit the result unless the map was closed or the image is gone
		// or changed again since the job was queued
		for(const auto &pMap : m_vpMaps)
		{
			if(pMap.get() != pJob->TargetMap())
				continue;
			auto &vImages = pMap->m_vpImages;
			const bool ImageStillThere = std::find_if(vImages.begin(), vImages.end(), [&](const auto &pImg) { return pImg.get() == pJob->TargetImage(); }) != vImages.end();
			if(ImageStillThere && pJob->TargetImage()->m_DataVersion == pJob->DataVersion())
				pMap->m_ImageCompressedSizes[pJob->TargetImage()] = {pJob->DataVersion(), pJob->CompressedSize()};
			break;
		}
		It = m_vImageSizeJobs.erase(It);
	}

	// keep the worker busy with one image at a time; the selected image has
	// priority, the rest is recomputed in order over the next frames
	if(m_vImageSizeJobs.size() >= 2)
		return;

	const CEditorImage *pStaleImage = nullptr;
	std::shared_ptr<CEditorImage> pSelected = Map()->SelectedImage();
	if(pSelected != nullptr && ImageEligibleForAccounting(pSelected.get()))
	{
		auto It = Map()->m_ImageCompressedSizes.find(pSelected.get());
		if(It == Map()->m_ImageCompressedSizes.end() || It->second.first != pSelected->m_DataVersion)
			pStaleImage = pSelected.get();
	}
	if(pStaleImage == nullptr)
	{
		for(const std::shared_ptr<CEditorImage> &pImg : Map()->m_vpImages)
		{
			if(!ImageEligibleForAccounting(pImg.get()))
				continue;
			auto It = Map()->m_ImageCompressedSizes.find(pImg.get());
			if(It == Map()->m_ImageCompressedSizes.end() || It->second.first != pImg->m_DataVersion)
			{
				pStaleImage = pImg.get();
				break;
			}
		}
	}
	if(pStaleImage != nullptr)
	{
		auto pJob = std::make_shared<CEditorImageSizeJob>(Map(), pStaleImage, pStaleImage->m_DataVersion, pStaleImage->m_pData, pStaleImage->DataSize());
		Engine()->AddJob(pJob);
		m_vImageSizeJobs.push_back(std::move(pJob));
	}
}

size_t CEditor::ImageDisplayedSize(const std::shared_ptr<CEditorImage> &pImage)
{
	if(pImage == nullptr || !ImageEligibleForAccounting(pImage.get()))
		return 0;
	auto It = Map()->m_ImageCompressedSizes.find(pImage.get());
	if(It != Map()->m_ImageCompressedSizes.end() && It->second.first == pImage->m_DataVersion)
		return It->second.second;
	// raw size until the compressed size has been computed in the background
	return pImage->DataSize();
}

size_t CEditor::ImagesTotalDisplayedSize()
{
	size_t Total = 0;
	for(const std::shared_ptr<CEditorImage> &pImg : Map()->m_vpImages)
		Total += ImageDisplayedSize(pImg);
	return Total;
}

size_t CEditor::SoundsTotalSize()
{
	size_t Total = 0;
	for(const std::shared_ptr<CEditorSound> &pSound : Map()->m_vpSounds)
	{
		if(pSound->m_pData != nullptr)
			Total += pSound->m_DataSize;
	}
	return Total;
}
