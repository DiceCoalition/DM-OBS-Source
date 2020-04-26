#include <obs-module.h>
#include <graphics/image-file.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <util/threading.h>
#include <util/darray.h>
#include <curl/curl.h>
#include <curl/easy.h>


#define blog(log_level, format, ...) \
	blog(log_level, "[dm_source: '%s'] " format, \
			obs_source_get_name(context->src), ##__VA_ARGS__)

#define debug(format, ...) \
	blog(LOG_DEBUG, format, ##__VA_ARGS__)
#define info(format, ...) \
	blog(LOG_INFO, format, ##__VA_ARGS__)
#define warn(format, ...) \
	blog(LOG_WARNING, format, ##__VA_ARGS__)

struct dm_source {
	obs_source_t *src;
	char *imagefolder;
	char *tbstring;
	uint32_t speed;
	DARRAY(char*) files;
	DARRAY(char*) dice;
	int currentIndex;
	uint64_t     last_time;
	float        update_time_elapsed;
	gs_image_file_t image;
	gs_image_file_t diceimage;
	gs_texture_t *comboTexture;
	bool visible;
	bool showdicecount;
	bool useplaymatlayout;
	uint32_t height;
};

bool ConvertCharToBitmap(TCHAR* szFileName, TCHAR* szStr, int iWidth, int iHeight, int iFontSize)
{
	HWND hWnd = GetActiveWindow();
	HDC hDC = GetDC(hWnd);

	HDC hMemDC = CreateCompatibleDC(hDC);
	SetTextColor(hMemDC, RGB(255, 255, 255));
	SetBkMode(hMemDC, TRANSPARENT);

	HBITMAP hBitmap = CreateCompatibleBitmap(hDC, iWidth, iHeight);
	HBITMAP hOldBitmap = (HBITMAP)SelectObject(hMemDC, hBitmap);

	HFONT hFont = CreateFont(iFontSize, 0, 0, 0, FW_DONTCARE, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
		CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, TEXT("Impact"));
	HFONT hOldFont = (HFONT)SelectObject(hMemDC, hFont);

	HBRUSH hBrush = CreateSolidBrush(RGB(0, 0, 0));
	HBRUSH hOldBrush = (HBRUSH)SelectObject(hMemDC, hBrush);

	RECT rc = { 0, 0, iWidth, iHeight };
	FillRect(hMemDC, &rc, hBrush);

	DrawText(hMemDC, szStr, -1, &rc, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

	TextOut(hMemDC, 0, 0, szStr, -1);

	BITMAP bmp;
	GetObject(hBitmap, sizeof(BITMAP), &bmp);

	BITMAPFILEHEADER   bmfHeader;
	BITMAPINFOHEADER   bi;

	bi.biSize = sizeof(BITMAPINFOHEADER);
	bi.biWidth = bmp.bmWidth;
	bi.biHeight = bmp.bmHeight;
	bi.biPlanes = 1;
	bi.biBitCount = 32;
	bi.biCompression = BI_RGB;
	bi.biSizeImage = 0;
	bi.biXPelsPerMeter = 0;
	bi.biYPelsPerMeter = 0;
	bi.biClrUsed = 0;
	bi.biClrImportant = 0;

	DWORD dwBmpSize = ((bmp.bmWidth * bi.biBitCount + 31) / 32) * 4 * bmp.bmHeight;

	HANDLE hDIB = GlobalAlloc(GHND, dwBmpSize);
	char *lpBitmap = (char *)GlobalLock(hDIB);

	// Gets the "bits" from the bitmap and copies them into a buffer 
	// which is pointed to by lpbitmap.
	GetDIBits(hMemDC, hBitmap, 0,
		(UINT)bmp.bmHeight,
		lpBitmap,
		(BITMAPINFO *)&bi, DIB_RGB_COLORS);

	// A file is created, this is where we will save the screen capture.
	HANDLE hFile = CreateFile(szFileName,
		GENERIC_WRITE,
		0,
		NULL,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, NULL);

	// Add the size of the headers to the size of the bitmap to get the total file size
	DWORD dwSizeofDIB = dwBmpSize + sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

	//Offset to where the actual bitmap bits start.
	bmfHeader.bfOffBits = (DWORD)sizeof(BITMAPFILEHEADER) + (DWORD)sizeof(BITMAPINFOHEADER);

	//Size of the file
	bmfHeader.bfSize = dwSizeofDIB;

	//bfType must always be BM for Bitmaps
	bmfHeader.bfType = 0x4D42; //BM   

	DWORD dwBytesWritten = 0;
	WriteFile(hFile, (LPSTR)&bmfHeader, sizeof(BITMAPFILEHEADER), &dwBytesWritten, NULL);
	WriteFile(hFile, (LPSTR)&bi, sizeof(BITMAPINFOHEADER), &dwBytesWritten, NULL);
	WriteFile(hFile, (LPSTR)lpBitmap, dwBmpSize, &dwBytesWritten, NULL);

	//Unlock and Free the DIB from the heap
	GlobalUnlock(hDIB);
	GlobalFree(hDIB);

	//Close the handle for the file that was created
	CloseHandle(hFile);

	//SelectObject(hMemDC, hOldBrush);
	SelectObject(hDC, hOldFont);
	SelectObject(hDC, hOldBitmap);

	//DeleteObject(hBrush);
	DeleteObject(hFont);
	DeleteObject(hBitmap);
	DeleteDC(hMemDC);

	ReleaseDC(NULL, hDC);

	return true;
}

size_t callbackfunction(void *ptr, size_t size, size_t nmemb, void* userdata)
{
	FILE* stream = (FILE*)userdata;
	if (!stream)
	{
		printf("!!! No stream\n");
		return 0;
	}

	size_t written = fwrite((FILE*)ptr, size, nmemb, stream);
	return written;
}

bool download_jpeg(char* url, char* destination)
{
	FILE* fp = fopen(destination, "wb");
	if (!fp)
	{
		printf("!!! Failed to create file on the disk\n");
		return false;
	}

	CURL* curlCtx = curl_easy_init();
	curl_easy_setopt(curlCtx, CURLOPT_URL, url);
	curl_easy_setopt(curlCtx, CURLOPT_WRITEDATA, fp);
	curl_easy_setopt(curlCtx, CURLOPT_WRITEFUNCTION, callbackfunction);
	curl_easy_setopt(curlCtx, CURLOPT_FOLLOWLOCATION, 1);

	CURLcode rc = curl_easy_perform(curlCtx);
	if (rc)
	{
		printf("!!! Failed to download: %s\n", url);
		return false;
	}

	long res_code = 0;
	curl_easy_getinfo(curlCtx, CURLINFO_RESPONSE_CODE, &res_code);
	if (!((res_code == 200 || res_code == 201) && rc != CURLE_ABORTED_BY_CALLBACK))
	{
		printf("!!! Response code: %d\n", res_code);
		return false;
	}

	curl_easy_cleanup(curlCtx);

	fclose(fp);

	return true;
}

bool updateFileList(struct dm_source *context)
{
	bool updated = false;
	char *tbstring = context->tbstring;
	int fileCount = context->files.num;
	if (fileCount > 0) {
		darray_erase_range(sizeof(context->files), &context->files.da, 0, fileCount);
		darray_erase_range(sizeof(context->dice), &context->dice.da, 0, fileCount);
	}

	int status = mkdir(context->imagefolder);

	if (tbstring && *tbstring) {
		debug("loading texture '%s'", tbstring);
		int i = 0;
		char *token;

		const char s[2] = ";";
		const char e[2] = "=";
		int count = 0;
		int ecount = 0;
		for (int j = 0; j < strlen(tbstring); j++) {
			if (tbstring[j] == ';') count++;
			else if (tbstring[j] == '=') ecount++;
		}

		struct dstr dcardlist = { 0 };
		//copy the list so we don't alter it
		dstr_copy(&dcardlist, tbstring);
		if (ecount > 0) {
			char* cardsString = dstr_find(&dcardlist, e);
			dstr_copy(&dcardlist, "");
			for (int t = 1; t < strlen(cardsString); t++) {
				if (cardsString[t] == '&')
					break;
				dstr_cat_ch(&dcardlist, cardsString[t]);
			}
		}
		token = strtok(dcardlist.array, s);

		while (token != NULL)
		{
			struct dstr dcard = { 0 };
			struct dstr dpath = { 0 };
			struct dstr dice = { 0 };
			char* folderpath = context->imagefolder;
			dstr_copy(&dpath, folderpath);
			dstr_cat_ch(&dpath, '/');
			//dstr_copy(&dice, "Dice: ");
			dstr_cat_ch(&dice, token[0]);

			for (int c = 2; c < strlen(token); c++) {
				dstr_cat_ch(&dcard, token[c]);
			}
			dstr_cat(&dpath, dcard.array);
			dstr_cat(&dpath, ".jpg");

			da_push_back(context->files, &dpath.array);

			//downlaod files if they don't exist
			FILE* fp = fopen(dpath.array, "r");
			if (fp) {
				fclose(fp);
			}
			else {
				struct dstr url = { 0 };
				int cnum;
				char set[10];
				if (dcard.array != NULL) {
					sscanf(dcard.array, "%d%s", &cnum, set);
					//todo can we regex  the check set
					char* buffer;
					if (strlen(set) < 8) {
						dstr_copy(&url, "http://dicecoalition.com/cardservice/Image.php?set=");
						dstr_cat(&url, set);
						dstr_cat(&url, "&cardnum=");
						char numString[5];
						if (cnum > 0 && cnum < 1000) {
							itoa(cnum, numString, 10);
							dstr_cat(&url, numString);
							dstr_cat(&url, "&res=l");
							download_jpeg(url.array, dpath.array);
						}

					}
				}
				dstr_free(&url);
			}
			//create Dice text BMP if it doesn't exist
			struct dstr diceImage = { 0 };
			dstr_copy(&diceImage, folderpath);
			dstr_cat_ch(&diceImage, '/');
			dstr_cat(&diceImage, "Dice");
			dstr_cat(&diceImage, dice.array);
			dstr_cat(&diceImage, ".jpg");
			FILE* fpdice = fopen(diceImage.array, "r");
			if (fpdice) {
				fclose(fpdice);
			}
			else {
				struct dstr diceurl = { 0 };
				int cnum;
				char set[10];
				if (dice.array != NULL) {
					dstr_copy(&diceurl, "http://dicecoalition.com/cardservice/Cards/Dice");
					dstr_cat(&diceurl, dice.array);
					dstr_cat(&diceurl, ".jpg");
					download_jpeg(diceurl.array, diceImage.array);
				}
				//TODO: Was trying to generate image on the fly.  Let's just download one instead
				/*
				struct dstr diceText = { 0 };
				dstr_copy(&diceText, "Dice: ");
				dstr_cat(&diceText, dice.array);
				const WCHAR fileName[100];
				int nChars = MultiByteToWideChar(CP_ACP, 0, diceImage.array, -1, NULL, 0);
				MultiByteToWideChar(CP_ACP, 0, diceImage.array, -1, (LPWSTR)fileName, nChars);
				const WCHAR diceString[10];
				nChars = MultiByteToWideChar(CP_ACP, 0, diceText.array, -1, NULL, 0);
				MultiByteToWideChar(CP_ACP, 0, diceText.array, -1, (LPWSTR)diceString, nChars);

				ConvertCharToBitmap(fileName, diceString, 368, 50, 40);
				*/
				dstr_free(&diceurl);
			}

			da_push_back(context->dice, &diceImage.array);
			//dstr_free(&diceImage);
			//dstr_free(&dcard);
			//dstr_free(&dpath);
			//dstr_free(&dice);
			i++;
			token = strtok(NULL, s);
		}
		updated = true;
	}
	return updated;

}

void updateTextures(struct dm_source *context) {
	if (context->useplaymatlayout)
	{		
		obs_enter_graphics();
		gs_image_file_free(&context->image);
		gs_image_file_free(&context->diceimage);
		if (context->comboTexture != NULL) {
			gs_texture_destroy(context->comboTexture);
			context->comboTexture = NULL;
		}
		obs_leave_graphics();

		char* file = context->files.array[0];
		gs_image_file_init(&context->image, file);

		obs_enter_graphics();
		gs_image_file_init_texture(&context->image);
		obs_leave_graphics();
		char* file = context->files.array[0];
		for (int i = 1; i < context->files.num; i++)
		{
			char* file = context->files.array[i];
			if (file == NULL)
				warn("Image list is empty");
			else {
			}
		}
	}
	else{
		char* file = context->files.array[context->currentIndex];
		if (file == NULL)
			warn("Image list is empty");
		else {
			obs_enter_graphics();
			gs_image_file_free(&context->image);
			gs_image_file_free(&context->diceimage);
			if (context->comboTexture != NULL) {
				gs_texture_destroy(context->comboTexture);
				context->comboTexture = NULL;
			}
			obs_leave_graphics();

			gs_image_file_init(&context->image, file);

			obs_enter_graphics();
			gs_image_file_init_texture(&context->image);
			obs_leave_graphics();

			if (!context->image.loaded) {
				warn("failed to load texture '%s'", file);
				//file list may of gotten corrupted by a bad update.  Try to re-parse
				updateFileList(context);
			}
			if (context->showdicecount)
			{
				gs_image_file_init(&context->diceimage, context->dice.array[context->currentIndex]);

				obs_enter_graphics();
				gs_image_file_init_texture(&context->diceimage);
				obs_leave_graphics();

				obs_enter_graphics();

				context->comboTexture = gs_texture_create_gdi(context->diceimage.cx, context->image.cy + context->diceimage.cy);

				gs_copy_texture_region(context->comboTexture, 0, 0, context->image.texture, 0, 0, context->diceimage.cx, context->image.cy);
				gs_copy_texture_region(context->comboTexture, 0, context->image.cy, context->diceimage.texture, 0, 0, context->diceimage.cx, context->diceimage.cy);
				obs_leave_graphics();

				if (!context->diceimage.loaded)
					warn("failed to load texture '%s'", context->dice.array[0]);
			}
		}
	}
}

static const char *dm_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Dice Masters Source");
}

static void dm_source_load(struct dm_source *context)
{
	char *tbstring = context->tbstring;
	context->currentIndex = 0;
	obs_enter_graphics();
	gs_image_file_free(&context->image);
	gs_image_file_free(&context->diceimage);
	obs_leave_graphics();

	bool updated = updateFileList(context);
	if (updated) {
		updateTextures(context);
	}

}

static void dm_source_unload(struct dm_source *context)
{
	obs_enter_graphics();
	gs_image_file_free(&context->image);
	gs_image_file_free(&context->diceimage);
	if (context->comboTexture != NULL) {		
		gs_texture_destroy(context->comboTexture);
		context->comboTexture = NULL;
	}
	obs_leave_graphics();
}

static void dm_source_update(void *data, obs_data_t *settings)
{
	struct dm_source *context = data;
	char* tbstring = (char*)obs_data_get_string(settings, "tbstring");
	char* imagefolder = (char*)obs_data_get_string(settings, "imagefolder");
	uint32_t speed = (uint32_t)obs_data_get_int(settings, "speed");
	bool dicecount = (bool)obs_data_get_bool(settings, "dicecount");
	bool playmat = (bool)obs_data_get_bool(settings, "useplaymat");

	context->imagefolder = imagefolder;
	context->tbstring = tbstring;
	context->speed = speed;
	context->showdicecount = dicecount;
	context->useplaymatlayout = playmat;
	dm_source_load(data);
}

static void *dm_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct dm_source *context = bzalloc(sizeof(struct dm_source));
	context->src = source;

	dm_source_update(context, settings);

	return context;
}

static void dm_source_destroy(void *data)
{
	struct dm_source *context = data;
	dm_source_unload(context);

	if (context->tbstring)
		bfree(context->tbstring);
	bfree(context);
}

static obs_properties_t *dm_source_properties(void *data)
{
	struct dm_source *s = data;
	//struct dstr path = { 0 };

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "tbstring", obs_module_text("Team Builder String"), OBS_TEXT_DEFAULT);
	obs_properties_add_path(props, "imagefolder", obs_module_text("Image Folder"), OBS_PATH_DIRECTORY, NULL, "c:/temp/cards");

	obs_properties_add_int(props, "speed", obs_module_text("Cycle Speed (s)"), 0, 4096, 1);
	obs_properties_add_bool(props, "dicecount", obs_module_text("Show Dice Count"));

	obs_properties_add_bool(props, "useplaymat", obs_module_text("Use Playmat Layout"));
	return props;
}

static void dm_source_render(void *data, gs_effect_t *effect)
{
	struct dm_source *context = data;

	if (!context->image.texture)
		return;
	if (!context->showdicecount) {
		gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"),
			context->image.texture);
		gs_draw_sprite(context->image.texture, 0,
			context->image.cx, context->image.cy);
	}
	else {
		gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"),
			context->comboTexture);
		gs_draw_sprite(context->comboTexture, 0,
			context->diceimage.cx, context->image.cy + context->diceimage.cy);
	}
}

static uint32_t dm_source_getwidth(void *data)
{
	struct dm_source *context = data;
	int width = context->image.cx;
	if (context->showdicecount) {
		width = context->diceimage.cx;
	}
	return width;
}

static uint32_t dm_source_getheight(void *data)
{
	struct dm_source *context = data;
	int height = context->image.cy;
	if (context->showdicecount) {
		height += context->diceimage.cy;
	}
	return height;
}

static void dm_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "tbstring", "4x75bff;2x78avx");
	obs_data_set_default_string(settings, "imagefolder", "c:/temp/cards");
	obs_data_set_default_int(settings, "speed", 10);
	obs_data_set_default_bool(settings, "dicecount", false);
	obs_data_set_default_bool(settings, "usePlaymat", false);
}
}

static void dm_source_show(void *data)
{
	struct dm_source *context = data;
	context->visible = true;
	//if (!context->persistent)
	dm_source_load(context);
}

static void dm_source_hide(void *data)
{
	struct dm_source *context = data;
	context->visible = false;
	//if (!context->persistent)
	dm_source_unload(context);
}

static void dm_source_tick(void *data, float seconds)
{
	struct dm_source *context = data;
	if (context->visible) {
		uint64_t frame_time = obs_get_video_frame_time();

		context->update_time_elapsed += seconds;

		if (context->update_time_elapsed >= context->speed) {
			context->update_time_elapsed = 0;
			context->currentIndex++;
			if (context->currentIndex >= context->files.num)
				context->currentIndex = 0;
			updateTextures(context);
		}
		context->last_time = frame_time;
	}
}



struct obs_source_info dm_source_info = {
	.id = "dm_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = dm_source_get_name,
	.create = dm_source_create,
	.destroy = dm_source_destroy,
	.update = dm_source_update,
	.get_defaults = dm_source_defaults,
	.show = dm_source_show,
	.hide = dm_source_hide,
	.get_width = dm_source_getwidth,
	.get_height = dm_source_getheight,
	.video_render = dm_source_render,
	.video_tick = dm_source_tick,
	.get_properties = dm_source_properties
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("dm-source", "en-US")


bool obs_module_load(void)
{
	obs_register_source(&dm_source_info);
	return true;
}

