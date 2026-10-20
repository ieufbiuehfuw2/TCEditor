#ifndef GAME_EDITOR_BULK_COMPRESS_H
#define GAME_EDITOR_BULK_COMPRESS_H

#include <base/types.h>

#include <engine/shared/jobs.h>

#include <atomic>
#include <cstddef>
#include <vector>

class CEditorImage;
class CEditorMap;
class CEditorSound;

// Background computation of the deflate level 9 size of an embedded image's
// pixel data, exactly as the map writer produces it for its data blob. Takes
// a snapshot of the data, so the image can safely change while it runs.
class CEditorImageSizeJob : public IJob
{
public:
	CEditorImageSizeJob(CEditorMap *pMap, const CEditorImage *pImage, uint64_t DataVersion, const void *pData, size_t DataSize);

	void Run() override;

	CEditorMap *TargetMap() const { return m_pMap; }
	const CEditorImage *TargetImage() const { return m_pImage; }
	uint64_t DataVersion() const { return m_DataVersion; }
	bool Done() const { return m_Done.load(); }
	size_t CompressedSize() const { return m_CompressedSize; }

private:
	CEditorMap *m_pMap;
	const CEditorImage *m_pImage;
	uint64_t m_DataVersion;
	std::vector<uint8_t> m_vData;
	std::atomic<bool> m_Done{false};
	size_t m_CompressedSize = 0;
};

// Re-creates the GPU texture of an image from its current pixel data.
void RefreshImageTexture(CEditorImage *pImage);
// Replaces the pixel buffer and dimensions of an image (reallocating its
// data), recomputes its tile flags and reloads its GPU texture.
void ApplyImageData(CEditorImage *pImage, const std::vector<uint8_t> &vData, size_t Width, size_t Height);
// Replaces the memory buffer of a sound and reloads its sample.
void ApplySoundData(CEditorSound *pSound, const void *pData, unsigned DataSize);

// Zeroes pixels of an embedded image that no layer of the map can ever sample
// (per-pixel quad usage rasterization + 16x16 tile grid) and dilates the
// remaining transparent pixels, so the deflate stage of the datafile writer
// gets long runs of zeros. Size-neutral and visually lossless; the gain
// materializes when the map is saved.
void CompressEditorImagePixels(CEditorMap *pMap, int ImageIndex, CEditorImage *pImage);

// Decodes an ogg-opus sound via opusfile and re-encodes it with the libopus
// encoder at roughly half the original bitrate, muxed into a fresh ogg
// container. Returns false if the sound is not decodable opus or would not
// shrink.
bool CompressEditorSoundBytes(const void *pData, unsigned DataSize, std::vector<uint8_t> &rOut);

#endif // GAME_EDITOR_BULK_COMPRESS_H
