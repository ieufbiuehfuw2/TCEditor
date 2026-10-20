// TClient: online tilesets browser. Opened from the "Tilesets" menubar
// button, it lists the community mapres hosted on teedata.net. The catalog is
// fetched through the site's Next.js data route (the build id is extracted
// from the gallery page), thumbnails and full files are served from
// teedata.net/databasev2/<file_path>. Previews are downloaded lazily for
// visible entries, decoded on the main thread and uploaded as small
// textures. "Add" downloads the original quality image into the user's
// mapres folder and adds it to the map like a locally added image.
//
// teedata.net throttles every connection to roughly 24 KiB and then stalls,
// which kills any plain download of a full-size mapres through the
// low-speed timeout. Every request therefore sends "Connection: close" so
// curl opens a fresh connection each time, and full files are fetched in
// small ranged chunks (see CEditorTilesetsDownloadJob).

#include "editor.h"

#include <base/log.h>
#include <base/math.h>
#include <base/str.h>
#include <base/fs.h>
#include <base/io.h>

#include <engine/engine.h>
#include <engine/keys.h>
#include <engine/shared/json.h>
#include <engine/shared/jsonwriter.h>
#include <engine/storage.h>

#include <game/editor/mapitems/image.h>

#include <engine/shared/jobs.h>

#include <algorithm>
#include <string>
#include <vector>

static const char *const TEEDATA_BASE = "https://teedata.net";
static const char *const TEEDATA_DB_PREFIX = "/databasev2";
static constexpr int TEEDATA_MAX_PAGES = 40;
static constexpr int TEEDATA_MAX_PREVIEW_JOBS = 12;
static constexpr int TEEDATA_MAX_PREVIEW_ATTEMPTS = 2;
static constexpr int TEEDATA_MAX_DOWNLOAD_ATTEMPTS = 3;
static const char *const TEEDATA_CACHE_DIR = "editor/thumbcache";

// ---------------- catalog job ----------------

class CEditorTilesetsListJob : public IJob
{
public:
	CEditorTilesetsListJob(IHttp *pHttp) :
		m_pHttp(pHttp) {}

	void Run() override
	{
		FetchCatalog();
		m_Done = true;
	}

	bool Done() const { return m_Done.load(); }
	bool Failed() const { return m_Failed.load(); }
	const char *Error() const { return m_aError; }
	int TotalCount() const { return m_TotalCount.load(); }
	int DeliveredCount() const { return m_Delivered.load(); }
	// hands over the assets collected since the last call (thread-safe)
	void TakePending(std::vector<CEditor::SOnlineAsset> &vOut)
	{
		std::unique_lock Lock(m_PendingMutex);
		if(m_vPending.empty())
			return;
		vOut.insert(vOut.end(), std::make_move_iterator(m_vPending.begin()), std::make_move_iterator(m_vPending.end()));
		m_vPending.clear();
	}

private:
	bool FetchUrl(const char *pUrl, std::shared_ptr<IHttpRequest> &pRequest)
	{
		pRequest = HttpGet(pUrl);
		pRequest->Header("Connection: close");
		pRequest->WriteToMemory();
		pRequest->Timeout(CTimeout{10000, 60000, 200, 20});
		pRequest->LogProgress(HTTPLOG::FAILURE);
		m_pHttp->Run(pRequest);
		pRequest->Wait();
		return pRequest->State() == EHttpState::DONE;
	}

	void FetchCatalog()
	{
		// the gallery page reveals the current Next.js build id
		std::shared_ptr<IHttpRequest> pRequest;
		if(!FetchUrl("https://teedata.net/mapres", pRequest))
		{
			str_copy(m_aError, "Failed to reach teedata.net. Check your connection.");
			m_Failed = true;
			return;
		}
		unsigned char *pData = nullptr;
		size_t Len = 0;
		pRequest->Result(&pData, &Len);
		const char *pBegin = (const char *)pData;
		const char *pEnd = pBegin + Len;
		const char *pNeedle = "\"buildId\":\"";
		const char *pFound = std::search(pBegin, pEnd, pNeedle, pNeedle + str_length(pNeedle));
		char aBuildId[64] = "";
		if(pFound != pEnd)
		{
			const char *pIdStart = pFound + str_length(pNeedle);
			size_t IdLen = 0;
			while(pIdStart + IdLen < pEnd && pIdStart[IdLen] != '"' && IdLen < sizeof(aBuildId) - 1)
			{
				aBuildId[IdLen] = pIdStart[IdLen];
				IdLen++;
			}
			aBuildId[IdLen] = '\0';
		}
		if(aBuildId[0] == '\0')
		{
			str_copy(m_aError, "Could not parse the teedata.net catalog page.");
			m_Failed = true;
			return;
		}

		// fetch the catalog pages (30 assets each); every page appends its
		// entries so the dialog can show them while the rest is still loading.
		// The data route returns {"pageProps":{...}} at the top level, while
		// the SSR page nests it under {"props":{"pageProps":...}} - accept both
		char aUrl[256];
		int Page = 1;
		int Total = -1;
		int Collected = 0;
		while(Page <= TEEDATA_MAX_PAGES)
		{
			str_format(aUrl, sizeof(aUrl), "https://teedata.net/_next/data/%s/mapres.json?page=%d", aBuildId, Page);
			json_value *pJson = nullptr;
			bool Fetched = false;
			for(int Attempt = 0; Attempt < 2 && !Fetched; ++Attempt)
			{
				if(!FetchUrl(aUrl, pRequest))
					continue;
				pJson = pRequest->ResultJson();
				Fetched = pJson != nullptr && pJson->type == json_object;
			}
			if(!Fetched)
			{
				log_info("editor/tilesets", "catalog page %d could not be fetched or parsed", Page);
				break;
			}
			const json_value *pPageProps = json_object_get(pJson, "pageProps");
			if(pPageProps == nullptr || pPageProps->type != json_object)
			{
				const json_value *pProps = json_object_get(pJson, "props");
				pPageProps = pProps != nullptr && pProps->type == json_object ? json_object_get(pProps, "pageProps") : nullptr;
			}
			const json_value *pAssets = pPageProps != nullptr && pPageProps->type == json_object ? json_object_get(pPageProps, "assets") : nullptr;
			const json_value *pItems = pAssets != nullptr && pAssets->type == json_object ? json_object_get(pAssets, "items") : nullptr;
			if(pItems == nullptr || pItems->type != json_array)
			{
				log_info("editor/tilesets", "catalog page %d has no items array", Page);
				json_value_free(pJson);
				break;
			}
			if(Page == 1)
			{
				const json_value *pTotal = json_object_get(pAssets, "totalCount");
				if(pTotal != nullptr && pTotal->type == json_integer)
				{
					m_TotalCount = (int)pTotal->u.integer;
					Total = (int)pTotal->u.integer;
				}
			}

			int Added = 0;
			{
				std::unique_lock Lock(m_PendingMutex);
				for(size_t i = 0; i < pItems->u.array.length; ++i)
				{
					const json_value *pItem = pItems->u.array.values[i];
					if(pItem == nullptr || pItem->type != json_object)
						continue;
					const json_value *pFilePath = json_object_get(pItem, "file_path");
					if(pFilePath == nullptr || pFilePath->type != json_string || !str_startswith(pFilePath->u.string.ptr, "/mapres/"))
						continue; // only mapres images

					CEditor::SOnlineAsset Asset;
					const json_value *pVal = json_object_get(pItem, "name");
					if(pVal != nullptr && pVal->type == json_string)
						str_copy(Asset.m_aName, pVal->u.string.ptr, sizeof(Asset.m_aName));
					str_copy(Asset.m_aFilePath, pFilePath->u.string.ptr, sizeof(Asset.m_aFilePath));
					const json_value *pThumb = json_object_get(pItem, "thumbnail_path");
					if(pThumb != nullptr && pThumb->type == json_string)
						str_copy(Asset.m_aThumbnailPath, pThumb->u.string.ptr, sizeof(Asset.m_aThumbnailPath));
					const json_value *pWidth = json_object_get(pItem, "width");
					if(pWidth != nullptr && pWidth->type == json_integer)
						Asset.m_Width = (int)pWidth->u.integer;
					const json_value *pHeight = json_object_get(pItem, "height");
					if(pHeight != nullptr && pHeight->type == json_integer)
						Asset.m_Height = (int)pHeight->u.integer;
					const json_value *pSize = json_object_get(pItem, "size");
					if(pSize != nullptr && pSize->type == json_integer)
						Asset.m_Size = pSize->u.integer;
					m_vPending.push_back(std::move(Asset));
					Added++;
				}
			}
			m_Delivered += Added;
			Collected += Added;
			json_value_free(pJson);
			if(Added == 0 || (Total >= 0 && Collected >= Total))
				break;
			Page++;
		}

		if(Collected == 0)
		{
			str_copy(m_aError, "The teedata.net catalog is empty or unavailable.");
			m_Failed = true;
		}
	}

	IHttp *m_pHttp;
	std::atomic<bool> m_Done{false};
	std::atomic<bool> m_Failed{false};
	std::atomic<int> m_TotalCount{-1};
	std::atomic<int> m_Delivered{0};
	std::mutex m_PendingMutex;
	std::vector<CEditor::SOnlineAsset> m_vPending;
	char m_aError[256] = "";
};

// ---------------- preview job ----------------

class CEditorTilesetsPreviewJob : public IJob
{
public:
	CEditorTilesetsPreviewJob(IHttp *pHttp, IStorage *pStorage, const char *pUrl, const char *pCachePath, int AssetIndex) :
		m_pHttp(pHttp),
		m_pStorage(pStorage),
		m_AssetIndex(AssetIndex)
	{
		str_copy(m_aUrl, pUrl);
		str_copy(m_aCachePath, pCachePath);
	}

	void Run() override
	{
		// serve from the disk cache when possible
		IOHANDLE File = m_pStorage->OpenFile(m_aCachePath, IOFLAG_READ, IStorage::TYPE_SAVE);
		if(File != nullptr)
		{
			const int Length = io_length(File);
			if(Length > 0)
			{
				std::vector<unsigned char> vData(Length);
				if(io_read(File, vData.data(), Length) == (unsigned)Length)
				{
					m_vData = std::move(vData);
					m_Success = true;
				}
			}
			io_close(File);
			if(m_Success)
			{
				m_Done = true;
				return;
			}
		}

		// download and populate the cache
		std::shared_ptr<IHttpRequest> pRequest = HttpGet(m_aUrl);
		pRequest->Header("Connection: close");
		pRequest->WriteToMemory();
		pRequest->Timeout(CTimeout{4000, 25000, 500, 5});
		pRequest->LogProgress(HTTPLOG::FAILURE);
		m_pHttp->Run(pRequest);
		pRequest->Wait();
		if(pRequest->State() == EHttpState::DONE)
		{
			unsigned char *pData = nullptr;
			size_t Len = 0;
			pRequest->Result(&pData, &Len);
			if(Len > 4 && pData[0] == 0x89 && pData[1] == 'P' && pData[2] == 'N' && pData[3] == 'G')
			{
				m_vData.assign(pData, pData + Len);
				m_Success = true;
				IOHANDLE Out = m_pStorage->OpenFile(m_aCachePath, IOFLAG_WRITE, IStorage::TYPE_SAVE);
				if(Out != nullptr)
				{
					io_write(Out, m_vData.data(), m_vData.size());
					io_close(Out);
				}
			}
		}
		m_Done = true;
	}

	bool Done() const { return m_Done.load(); }
	bool Success() const { return m_Success; }
	int AssetIndex() const { return m_AssetIndex; }
	const std::vector<unsigned char> &Data() const { return m_vData; }

private:
	IHttp *m_pHttp;
	IStorage *m_pStorage;
	char m_aUrl[256] = "";
	char m_aCachePath[IO_MAX_PATH_LENGTH] = "";
	int m_AssetIndex;
	std::atomic<bool> m_Done{false};
	bool m_Success = false;
	std::vector<unsigned char> m_vData;
};

// ---------------- download job ----------------

// Downloads a full-size mapres in small ranged chunks, each on its own
// connection ("Connection: close"), because teedata.net stalls every
// connection after ~24 KiB. Chunked requests below that limit are served
// at full speed.
class CEditorTilesetsDownloadJob : public IJob
{
public:
	CEditorTilesetsDownloadJob(IHttp *pHttp, IStorage *pStorage, const char *pUrl, const char *pDest, int64_t ExpectedSize, const char *pName) :
		m_pHttp(pHttp),
		m_pStorage(pStorage),
		m_ExpectedSize(ExpectedSize)
	{
		str_copy(m_aUrl, pUrl);
		str_copy(m_aDest, pDest);
		str_copy(m_aName, pName);
	}

	void Run() override
	{
		constexpr int64_t ChunkSize = 16 * 1024;
		int64_t Offset = 0;
		int FailedChunks = 0;
		while(m_aError[0] == '\0')
		{
			const int64_t Remaining = m_ExpectedSize > Offset ? m_ExpectedSize - Offset : ChunkSize;
			const int64_t Chunk = std::min(Remaining, ChunkSize);
			char aRange[64];
			str_format(aRange, sizeof(aRange), "Range: bytes=%lld-%lld", (long long)Offset, (long long)(Offset + Chunk - 1));

			std::shared_ptr<IHttpRequest> pRequest = HttpGet(m_aUrl);
			pRequest->Header(aRange);
			pRequest->Header("Connection: close");
			pRequest->WriteToMemory();
			pRequest->Timeout(CTimeout{5000, 15000, 1000, 5});
			pRequest->LogProgress(HTTPLOG::FAILURE);
			m_pHttp->Run(pRequest);
			pRequest->Wait();

			size_t Len = 0;
			unsigned char *pData = nullptr;
			bool ChunkOk = false;
			if(pRequest->State() == EHttpState::DONE)
			{
				const int StatusCode = pRequest->StatusCode();
				if(StatusCode == 206 || StatusCode == 200)
				{
					pRequest->Result(&pData, &Len);
					ChunkOk = Len > 0 || Chunk <= 0;
				}
			}
			if(!ChunkOk)
			{
				FailedChunks++;
				if(FailedChunks > TEEDATA_MAX_DOWNLOAD_ATTEMPTS)
				{
					log_info("editor/tilesets", "download of '%s' failed at offset %lld", m_aName, (long long)Offset);
					str_copy(m_aError, "Download failed. teedata.net did not serve the file, try again.");
				}
				continue;
			}
			FailedChunks = 0;
			if(pRequest->StatusCode() == 200)
			{
				// range header ignored, the response contains the file from byte 0
				m_vData.clear();
				Offset = 0;
			}
			m_vData.insert(m_vData.end(), pData, pData + Len);
			Offset += Len;
			m_ProgressPercent.store(m_ExpectedSize > 0 ? (int)(Offset * 100 / m_ExpectedSize) : 0, std::memory_order_relaxed);
			if((m_ExpectedSize > 0 && Offset >= m_ExpectedSize) || Len < (size_t)Chunk)
				break; // complete, or short read means the last chunk
		}

		if(m_aError[0] == '\0')
		{
			const bool ValidPng = m_vData.size() > 4 && m_vData[0] == 0x89 && m_vData[1] == 'P' && m_vData[2] == 'N' && m_vData[3] == 'G';
			const bool SizeOk = m_ExpectedSize <= 0 || (int64_t)m_vData.size() == m_ExpectedSize;
			IOHANDLE File = ValidPng && SizeOk ? m_pStorage->OpenFile(m_aDest, IOFLAG_WRITE, IStorage::TYPE_SAVE) : nullptr;
			if(File != nullptr)
			{
				io_write(File, m_vData.data(), m_vData.size());
				io_close(File);
				m_Success = true;
			}
			else
				str_copy(m_aError, ValidPng ? "Downloaded file has an unexpected size, try again." : "Download finished, but the file is not a valid png.");
		}
		m_Done = true;
	}

	bool Done() const { return m_Done.load(); }
	bool Success() const { return m_Success; }
	const char *Error() const { return m_aError; }
	int ProgressPercent() const { return m_ProgressPercent.load(std::memory_order_relaxed); }

private:
	IHttp *m_pHttp;
	IStorage *m_pStorage;
	char m_aUrl[256] = "";
	char m_aDest[IO_MAX_PATH_LENGTH] = "";
	char m_aName[128] = "";
	int64_t m_ExpectedSize;
	std::atomic<bool> m_Done{false};
	std::atomic<int> m_ProgressPercent{0};
	bool m_Success = false;
	char m_aError[256] = "";
	std::vector<unsigned char> m_vData;
};

// ---------------- editor entry points ----------------

bool CEditor::LoadTilesetsCatalogCache()
{
	char aCatalogPath[IO_MAX_PATH_LENGTH];
	str_format(aCatalogPath, sizeof(aCatalogPath), "%s/catalog.json", TEEDATA_CACHE_DIR);
	IOHANDLE File = Storage()->OpenFile(aCatalogPath, IOFLAG_READ, IStorage::TYPE_SAVE);
	if(File == nullptr)
		return false;
	const int Length = io_length(File);
	std::vector<char> vBuffer(Length + 1);
	bool Loaded = false;
	if(Length > 0 && io_read(File, vBuffer.data(), Length) == (unsigned)Length)
	{
		vBuffer[Length] = '\0';
		json_value *pJson = JsonParse(vBuffer.data(), Length);
		if(pJson != nullptr && pJson->type == json_object)
		{
			const json_value *pAssets = json_object_get(pJson, "assets");
			if(pAssets != nullptr && pAssets->type == json_array)
			{
				for(size_t i = 0; i < pAssets->u.array.length; ++i)
				{
					const json_value *pItem = pAssets->u.array.values[i];
					if(pItem == nullptr || pItem->type != json_object)
						continue;
					SOnlineAsset Asset;
					const json_value *pVal = json_object_get(pItem, "name");
					if(pVal == nullptr || pVal->type != json_string)
						continue;
					str_copy(Asset.m_aName, pVal->u.string.ptr, sizeof(Asset.m_aName));
					pVal = json_object_get(pItem, "file_path");
					if(pVal == nullptr || pVal->type != json_string)
						continue;
					str_copy(Asset.m_aFilePath, pVal->u.string.ptr, sizeof(Asset.m_aFilePath));
					pVal = json_object_get(pItem, "thumbnail_path");
					if(pVal != nullptr && pVal->type == json_string)
						str_copy(Asset.m_aThumbnailPath, pVal->u.string.ptr, sizeof(Asset.m_aThumbnailPath));
					pVal = json_object_get(pItem, "width");
					if(pVal != nullptr && pVal->type == json_integer)
						Asset.m_Width = (int)pVal->u.integer;
					pVal = json_object_get(pItem, "height");
					if(pVal != nullptr && pVal->type == json_integer)
						Asset.m_Height = (int)pVal->u.integer;
					pVal = json_object_get(pItem, "size");
					if(pVal != nullptr && pVal->type == json_integer)
						Asset.m_Size = pVal->u.integer;
					m_vOnlineAssets.push_back(std::move(Asset));
				}
				Loaded = !m_vOnlineAssets.empty();
			}
			json_value_free(pJson);
		}
	}
	io_close(File);
	return Loaded;
}

void CEditor::SaveTilesetsCatalogCache()
{
	char aCatalogPath[IO_MAX_PATH_LENGTH];
	str_format(aCatalogPath, sizeof(aCatalogPath), "%s/catalog.json", TEEDATA_CACHE_DIR);
	Storage()->CreateFolder(TEEDATA_CACHE_DIR, IStorage::TYPE_SAVE);
	IOHANDLE File = Storage()->OpenFile(aCatalogPath, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(File == nullptr)
		return;
	CJsonFileWriter Writer(File);
	Writer.BeginObject();
	Writer.WriteAttribute("assets");
	Writer.BeginArray();
	for(const SOnlineAsset &Asset : m_vOnlineAssets)
	{
		Writer.BeginObject();
		Writer.WriteAttribute("name");
		Writer.WriteStrValue(Asset.m_aName);
		Writer.WriteAttribute("file_path");
		Writer.WriteStrValue(Asset.m_aFilePath);
		Writer.WriteAttribute("thumbnail_path");
		Writer.WriteStrValue(Asset.m_aThumbnailPath);
		Writer.WriteAttribute("width");
		Writer.WriteIntValue(Asset.m_Width);
		Writer.WriteAttribute("height");
		Writer.WriteIntValue(Asset.m_Height);
		Writer.WriteAttribute("size");
		Writer.WriteIntValue((int)Asset.m_Size);
		Writer.EndObject();
	}
	Writer.EndArray();
	Writer.EndObject();
	io_close(File);
}

void CEditor::OpenTilesetsBrowser()
{
	Storage()->CreateFolder("editor", IStorage::TYPE_SAVE);
	Storage()->CreateFolder(TEEDATA_CACHE_DIR, IStorage::TYPE_SAVE);
	if(m_vOnlineAssets.empty() && m_pTilesetsListJob == nullptr)
	{
		if(LoadTilesetsCatalogCache())
		{
			str_format(m_aTilesetsStatus, sizeof(m_aTilesetsStatus), "Catalog loaded from cache (%d mapres). Press Refresh to update it from teedata.net.", (int)m_vOnlineAssets.size());
			m_TilesetsStatusError = false;
		}
		else
		{
			m_pTilesetsListJob = std::make_shared<CEditorTilesetsListJob>(Kernel()->RequestInterface<IHttp>());
			Engine()->AddJob(m_pTilesetsListJob);
			str_copy(m_aTilesetsStatus, "Loading the catalog from teedata.net...");
			m_TilesetsStatusError = false;
		}
	}
	m_TilesetsSelected = -1;
	m_Dialog = DIALOG_TILESETS;
}

void CEditor::CloseTilesetsBrowser()
{
	UnloadTilesetsPreviews();
	m_TilesetsSelected = -1;
	OnDialogClose();
}

void CEditor::UnloadTilesetsPreviews()
{
	for(auto &Asset : m_vOnlineAssets)
	{
		Graphics()->UnloadTexture(&Asset.m_PreviewTexture);
		Asset.m_PreviewWanted = false;
		Asset.m_PreviewFailed = false;
		Asset.m_PreviewAttempts = 0;
	}
}

void CEditor::StartTilesetsAdd(int Index)
{
	if(m_pTilesetsDownloadJob != nullptr)
		return; // a download is already in progress
	if(Index < 0 || Index >= (int)m_vOnlineAssets.size())
		return;
	const SOnlineAsset &Asset = m_vOnlineAssets[Index];
	if(Asset.m_aFilePath[0] == '\0')
		return;

	char aUrl[256];
	str_format(aUrl, sizeof(aUrl), "%s%s%s", TEEDATA_BASE, TEEDATA_DB_PREFIX, Asset.m_aFilePath);
	str_format(m_aTilesetsDownloadDest, sizeof(m_aTilesetsDownloadDest), "mapres/%s.png", Asset.m_aName);
	m_TilesetsDownloadIndex = Index;
	m_pTilesetsDownloadJob = std::make_shared<CEditorTilesetsDownloadJob>(Kernel()->RequestInterface<IHttp>(), Storage(), aUrl, m_aTilesetsDownloadDest, Asset.m_Size, Asset.m_aName);
	Engine()->AddJob(m_pTilesetsDownloadJob);
	str_format(m_aTilesetsStatus, sizeof(m_aTilesetsStatus), "Downloading '%s'...", Asset.m_aName);
	m_TilesetsStatusError = false;
}

void CEditor::RenderTilesetsBrowser()
{
	if(m_Dialog != DIALOG_TILESETS)
		return;

	Ui()->MapScreen();
	CUIRect View = *Ui()->Screen();
	Ui()->SetHotItem(this);
	View.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.75f), IGraphics::CORNER_NONE, 0.0f);
	View.VMargin(120.0f, &View);
	View.HMargin(30.0f, &View);
	View.Draw(EditorTheme::PANEL_COLOR, IGraphics::CORNER_ALL, 5.0f);
	View.Margin(10.0f, &View);

	if(Ui()->ConsumeHotkey(CUi::HOTKEY_ESCAPE))
	{
		CloseTilesetsBrowser();
		return;
	}

	// catalog job: take new entries and the final state
	if(m_pTilesetsListJob != nullptr)
	{
		std::vector<SOnlineAsset> vNew;
		m_pTilesetsListJob->TakePending(vNew);
		if(!vNew.empty())
			m_vOnlineAssets.insert(m_vOnlineAssets.end(), std::make_move_iterator(vNew.begin()), std::make_move_iterator(vNew.end()));
		if(m_pTilesetsListJob->Done())
		{
			if(m_pTilesetsListJob->Failed())
			{
				str_copy(m_aTilesetsStatus, m_pTilesetsListJob->Error());
				m_TilesetsStatusError = true;
			}
			else
			{
				SaveTilesetsCatalogCache();
				str_format(m_aTilesetsStatus, sizeof(m_aTilesetsStatus), "Catalog loaded: %d mapres from teedata.net.", (int)m_vOnlineAssets.size());
				m_TilesetsStatusError = false;
			}
			m_pTilesetsListJob = nullptr;
		}
	}

	// preview jobs: decode finished downloads and create their textures
	int Processed = 0;
	for(auto It = m_vTilesetsPreviewJobs.begin(); It != m_vTilesetsPreviewJobs.end();)
	{
		if((*It)->Done() && Processed < 8)
		{
			const int AssetIndex = (*It)->AssetIndex();
			if(AssetIndex >= 0 && AssetIndex < (int)m_vOnlineAssets.size())
			{
				SOnlineAsset &Asset = m_vOnlineAssets[AssetIndex];
				if((*It)->Success() && !(*It)->Data().empty())
				{
					CImageInfo Img;
					if(Graphics()->LoadPng(Img, (*It)->Data().data(), (*It)->Data().size(), Asset.m_aName))
						Asset.m_PreviewTexture = Graphics()->LoadTextureRaw(Img, 0, Asset.m_aName);
					else
						Asset.m_PreviewFailed = true;
				}
				else
					Asset.m_PreviewFailed = true;
				if(Asset.m_PreviewFailed)
					Asset.m_PreviewWanted = false; // allow a retry while attempts remain
			}
			It = m_vTilesetsPreviewJobs.erase(It);
			Processed++;
		}
		else
			++It;
	}

	// mapres download poll
	if(m_pTilesetsDownloadJob != nullptr && m_pTilesetsDownloadJob->Done())
	{
		char aDest[IO_MAX_PATH_LENGTH];
		str_copy(aDest, m_aTilesetsDownloadDest);
		const bool Ok = m_pTilesetsDownloadJob->Success();
		char aError[256];
		str_copy(aError, m_pTilesetsDownloadJob->Error());
		m_pTilesetsDownloadJob = nullptr;
		if(Ok)
		{
			// AddImage closes the active dialog on success; keep the browser open
			const bool Added = AddImage(aDest, IStorage::TYPE_SAVE, this);
			m_Dialog = DIALOG_TILESETS;
			if(Added)
			{
				str_format(m_aTilesetsStatus, sizeof(m_aTilesetsStatus), "Added '%s' to the map.", fs_filename(aDest));
				m_TilesetsStatusError = false;
			}
			else
			{
				str_format(m_aTilesetsStatus, sizeof(m_aTilesetsStatus), "Failed to add '%s' (already in the map or invalid).", fs_filename(aDest));
				m_TilesetsStatusError = true;
			}
		}
		else
		{
			str_copy(m_aTilesetsStatus, aError[0] != '\0' ? aError : "Download failed. Check your connection and try again.");
			m_TilesetsStatusError = true;
		}
	}

	// ---------------- top bar ----------------
	CUIRect TopBar, Title;
	View.HSplitTop(20.0f, &TopBar, &View);
	View.HSplitTop(5.0f, nullptr, &View);
	TopBar.VSplitLeft(160.0f, &Title, &TopBar);
	Ui()->DoLabel(&Title, "Online tilesets", 12.0f, TEXTALIGN_ML);

	// filter
	CUIRect FilterLabel, FilterBox;
	TopBar.VSplitLeft(5.0f, nullptr, &TopBar);
	TopBar.VSplitLeft(45.0f, &FilterLabel, &TopBar);
	TopBar.VSplitLeft(220.0f, &FilterBox, &TopBar);
	Ui()->DoLabel(&FilterLabel, "Search:", 10.0f, TEXTALIGN_ML);
	static CLineInput s_TilesetsFilterInput;
	s_TilesetsFilterInput.SetBuffer(m_aTilesetsFilter, sizeof(m_aTilesetsFilter));
	DoEditBox(&s_TilesetsFilterInput, &FilterBox, 10.0f, IGraphics::CORNER_ALL, "Filter by name.");

	CUIRect RefreshButton, CloseButton;
	TopBar.VSplitRight(60.0f, &TopBar, &CloseButton);
	TopBar.VSplitRight(5.0f, &TopBar, nullptr);
	TopBar.VSplitRight(80.0f, &TopBar, &RefreshButton);
	static int s_TilesetsRefreshButton = 0;
	if(DoButton_Editor(&s_TilesetsRefreshButton, "Refresh", 0, &RefreshButton, BUTTONFLAG_LEFT, "Re-download the catalog from teedata.net."))
	{
		UnloadTilesetsPreviews();
		m_vOnlineAssets.clear();
		m_TilesetsSelected = -1;
		m_pTilesetsListJob = std::make_shared<CEditorTilesetsListJob>(Kernel()->RequestInterface<IHttp>());
		Engine()->AddJob(m_pTilesetsListJob);
		str_copy(m_aTilesetsStatus, "Loading the catalog from teedata.net...");
		m_TilesetsStatusError = false;
	}
	static int s_TilesetsCloseButton = 0;
	if(DoButton_Editor(&s_TilesetsCloseButton, "Close", 0, &CloseButton, BUTTONFLAG_LEFT, "Close the browser."))
	{
		CloseTilesetsBrowser();
		return;
	}

	// status line
	CUIRect StatusRect;
	View.HSplitTop(14.0f, &StatusRect, &View);
	View.HSplitTop(4.0f, nullptr, &View);
	if(m_pTilesetsDownloadJob != nullptr)
	{
		char aStatus[256];
		str_format(aStatus, sizeof(aStatus), "Downloading '%s'... %d%%", fs_filename(m_aTilesetsDownloadDest), m_pTilesetsDownloadJob->ProgressPercent());
		TextRender()->TextColor(ColorRGBA(0.6f, 0.85f, 1.0f, 1.0f));
		Ui()->DoLabel(&StatusRect, aStatus, 10.0f, TEXTALIGN_ML);
		TextRender()->TextColor(TextRender()->DefaultTextColor());
	}
	else if(m_aTilesetsStatus[0] != '\0')
	{
		TextRender()->TextColor(m_TilesetsStatusError ? ColorRGBA(1.0f, 0.4f, 0.35f, 1.0f) : ColorRGBA(0.5f, 0.9f, 0.5f, 1.0f));
		Ui()->DoLabel(&StatusRect, m_aTilesetsStatus, 10.0f, TEXTALIGN_ML, {.m_MaxWidth = StatusRect.w, .m_EllipsisAtEnd = true});
		TextRender()->TextColor(TextRender()->DefaultTextColor());
	}
	else if(m_pTilesetsListJob != nullptr)
	{
		TextRender()->TextColor(ColorRGBA(0.7f, 0.7f, 0.75f, 1.0f));
		Ui()->DoLabel(&StatusRect, "Loading the catalog...", 10.0f, TEXTALIGN_ML);
		TextRender()->TextColor(TextRender()->DefaultTextColor());
	}

	// ---------------- asset list ----------------
	std::vector<int> vVisible;
	{
		CScrollRegionParams ScrollParams;
		ScrollParams.m_ScrollbarThickness = 10.0f;
		ScrollParams.m_ScrollbarMargin = 3.0f;
		ScrollParams.m_ScrollUnit = 112.0f;
		static CScrollRegion s_TilesetsScroll;
		s_TilesetsScroll.Begin(&View, &ScrollParams);

		for(int i = 0; i < (int)m_vOnlineAssets.size(); ++i)
		{
			const SOnlineAsset &Asset = m_vOnlineAssets[i];
			if(m_aTilesetsFilter[0] != '\0' && !str_find_nocase(Asset.m_aName, m_aTilesetsFilter))
				continue;

			CUIRect Row;
			View.HSplitTop(58.0f, &Row, &View);
			View.HSplitTop(2.0f, nullptr, &View);
			if(!s_TilesetsScroll.AddRect(Row))
				continue;
			vVisible.push_back(i);

			const bool Selected = m_TilesetsSelected == i;
			if(Selected)
			{
				// thin red ring: accent border punched with the panel color,
				// then the regular row sheen plus a faint red tint inside
				Row.Draw(EditorTheme::ACCENT.WithAlpha(0.9f), IGraphics::CORNER_ALL, 4.0f);
				CUIRect RowFill = Row;
				RowFill.Margin(1.0f, &RowFill);
				RowFill.Draw(EditorTheme::PANEL_COLOR, IGraphics::CORNER_ALL, 3.0f);
				RowFill.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.07f), IGraphics::CORNER_ALL, 3.0f);
				RowFill.Draw(EditorTheme::ACCENT.WithAlpha(0.10f), IGraphics::CORNER_ALL, 3.0f);
			}
			else
				Row.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.07f), IGraphics::CORNER_ALL, 4.0f);
			CUIRect RowInner = Row;
			RowInner.Margin(4.0f, &RowInner);

			// preview 48x48, aspect preserved
			CUIRect PreviewRect{RowInner.x, RowInner.y, 48.0f, RowInner.h};
			float PrevW = 48.0f;
			float PrevH = 48.0f;
			if(Asset.m_Width > 0 && Asset.m_Height > 0)
			{
				const float Scale = std::min(PrevW / Asset.m_Width, PrevH / Asset.m_Height);
				PrevW = std::max(8.0f, Asset.m_Width * Scale);
				PrevH = std::max(8.0f, Asset.m_Height * Scale);
			}
			CUIRect PreviewBox{PreviewRect.x + (48.0f - PrevW) / 2.0f, PreviewRect.y + (PrevH < PreviewRect.h ? (PreviewRect.h - PrevH) / 2.0f : 0.0f), PrevW, PrevH};
			PreviewBox.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.35f), IGraphics::CORNER_ALL, 2.0f);
			if(Asset.m_PreviewTexture.IsValid())
			{
				CUIRect PreviewInner = PreviewBox;
				PreviewInner.Margin(1.0f, &PreviewInner);
				Graphics()->TextureSet(Asset.m_PreviewTexture);
				Graphics()->WrapClamp();
				Graphics()->QuadsBegin();
				IGraphics::CQuadItem QuadItem(PreviewInner.x, PreviewInner.y, PreviewInner.w, PreviewInner.h);
				Graphics()->QuadsDrawTL(&QuadItem, 1);
				Graphics()->QuadsEnd();
				Graphics()->WrapNormal();
			}
			else if(Asset.m_PreviewFailed && Asset.m_PreviewAttempts >= TEEDATA_MAX_PREVIEW_ATTEMPTS)
				Ui()->DoLabel(&PreviewBox, "n/a", 8.0f, TEXTALIGN_MC);
			else
				Ui()->DoLabel(&PreviewBox, "...", 10.0f, TEXTALIGN_MC);

			// name + info
			CUIRect TextRect;
			RowInner.VSplitLeft(58.0f, nullptr, &TextRect);
			TextRect.VSplitRight(60.0f, &TextRect, nullptr);
			CUIRect NameRect, InfoRect;
			TextRect.HSplitMid(&NameRect, &InfoRect, 2.0f);
			Ui()->DoLabel(&NameRect, Asset.m_aName, 10.0f, TEXTALIGN_ML, {.m_MaxWidth = NameRect.w, .m_EllipsisAtEnd = true});
			char aInfo[128];
			if(Asset.m_Width > 0)
				str_format(aInfo, sizeof(aInfo), "%dx%d  •  %.1f KB", Asset.m_Width, Asset.m_Height, Asset.m_Size / 1024.0f);
			else
				str_copy(aInfo, "mapres asset");
			TextRender()->TextColor(ColorRGBA(0.65f, 0.65f, 0.7f, 1.0f));
			Ui()->DoLabel(&InfoRect, aInfo, 9.0f, TEXTALIGN_ML, {.m_MaxWidth = InfoRect.w, .m_EllipsisAtEnd = true});
			TextRender()->TextColor(TextRender()->DefaultTextColor());

			// select + add
			static int s_TilesetsRowIds[512] = {};
			static int s_TilesetsAddIds[512] = {};
			CUIRect SelectRect, AddArea;
			RowInner.VSplitRight(56.0f, &SelectRect, &AddArea);
			CUIRect AddButton;
			AddArea.VSplitRight(6.0f, &AddButton, nullptr);
			// pure click logic — the select zone must not paint its own
			// accent fill over the row highlight
			if(i < (int)std::size(s_TilesetsRowIds) && DoButtonLogic(&s_TilesetsRowIds[i], 0, &SelectRect, BUTTONFLAG_LEFT, "Select this asset."))
				m_TilesetsSelected = i;
			if(i < (int)std::size(s_TilesetsAddIds) && DoButton_Editor(&s_TilesetsAddIds[i], "Add", 0, &AddButton, BUTTONFLAG_LEFT, "Download the original quality image into your mapres folder and add it to the map.") && m_pTilesetsDownloadJob == nullptr)
				StartTilesetsAdd(i);
		}
		s_TilesetsScroll.End();
	}

	// spawn preview downloads for visible entries; the thumbnails always
	// live at /databasev2/mapres/<name>/<name>.thumbnail.png and are small
	// enough to arrive before the per-connection throttle kicks in. Runs
	// while the catalog is still streaming in so the first rows show their
	// previews immediately.
	{
		int InFlight = (int)m_vTilesetsPreviewJobs.size();
		for(int Index : vVisible)
		{
			if(InFlight >= TEEDATA_MAX_PREVIEW_JOBS)
				break;
			SOnlineAsset &Asset = m_vOnlineAssets[Index];
			if(Asset.m_PreviewWanted || Asset.m_PreviewTexture.IsValid())
				continue;
			if(Asset.m_PreviewAttempts >= TEEDATA_MAX_PREVIEW_ATTEMPTS)
				continue;
			if(Asset.m_aName[0] == '\0')
			{
				Asset.m_PreviewAttempts = TEEDATA_MAX_PREVIEW_ATTEMPTS;
				Asset.m_PreviewFailed = true;
				continue;
			}
			Asset.m_PreviewWanted = true;
			Asset.m_PreviewAttempts++;
			char aUrl[512];
			str_format(aUrl, sizeof(aUrl), "%s%s/mapres/%s/%s.thumbnail.png", TEEDATA_BASE, TEEDATA_DB_PREFIX, Asset.m_aName, Asset.m_aName);
			char aCachePath[IO_MAX_PATH_LENGTH];
			str_format(aCachePath, sizeof(aCachePath), "editor/thumbcache/%s.png", Asset.m_aName);
			auto pJob = std::make_shared<CEditorTilesetsPreviewJob>(Kernel()->RequestInterface<IHttp>(), Storage(), aUrl, aCachePath, Index);
			Engine()->AddJob(pJob);
			m_vTilesetsPreviewJobs.push_back(std::move(pJob));
			InFlight++;
		}
	}
}
