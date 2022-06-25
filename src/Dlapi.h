/******************************************************************************
*
*
* Notepad2
*
* Dlapi.h
*   Definitions for Directory Listing APIs
*
* See Readme.txt for more information about this source code.
* Please send me your comments to this work.
*
* See License.txt for details about distribution and modification.
*
*                                              (c) Florian Balmer 1996-2011
*                                                  florian.balmer@gmail.com
*                                               http://www.flos-freeware.ch
*
*
******************************************************************************/
#pragma once

//==== DirList ================================================================

//==== LV_ITEMDATA Structure ==================================================
typedef struct LV_ITEMDATA { // lvid
	LPITEMIDLIST pidl; // Item Id
	LPSHELLFOLDER lpsf; // Parent IShellFolder Interface
} LV_ITEMDATA, *LPLV_ITEMDATA;

typedef const LV_ITEMDATA * LPCLV_ITEMDATA;

//==== DlInit() ===============================================================
void DirList_Init(HWND hwnd, LPCWSTR pszHeader);

//==== DlDestroy() ============================================================
void DirList_Destroy(HWND hwnd);

//==== DlStartIconThread() ====================================================
void DirList_StartIconThread(HWND hwnd);

//==== DlFill() ===============================================================
#define DL_FOLDERS			32
#define DL_NONFOLDERS		64
#define DL_INCLHIDDEN		128
#define DL_ALLOBJECTS		(DL_FOLDERS | DL_NONFOLDERS | DL_INCLHIDDEN)
int DirList_Fill(HWND hwnd, LPCWSTR lpszDir, DWORD grfFlags, LPCWSTR lpszFileSpec,
				 bool bExcludeFilter, bool bNoFadeHidden, int iSortFlags, bool fSortRev);

//==== DlIconThread() =========================================================
DWORD WINAPI DirList_IconThread(LPVOID lpParam);

//==== DlGetDispInfo() ========================================================
bool DirList_GetDispInfo(HWND hwnd, LPARAM lParam, bool bNoFadeHidden);

//==== DlDeleteItem() =========================================================
bool DirList_DeleteItem(HWND hwnd, LPARAM lParam);

//==== DlSort() ===============================================================

#define DS_NAME			0
#define DS_SIZE			1
#define DS_TYPE			2
#define DS_LASTMOD		3

BOOL DirList_Sort(HWND hwnd, int lFlags, bool fRev);

//==== DlGetItem() ============================================================

#define DLE_NONE		0
#define DLE_DIR			1
#define DLE_FILE		2

#define DLI_FILENAME	1
#define DLI_DISPNAME	2
#define DLI_TYPE		4
#define DLI_ALL			(DLI_FILENAME | DLI_DISPNAME | DLI_TYPE)

typedef struct DLITEM {	// dli
	UINT mask;
	WCHAR szFileName[MAX_PATH];
	WCHAR szDisplayName[MAX_PATH];
	int ntype;
} DLITEM, *LPDLITEM;

int DirList_GetItem(HWND hwnd, int iItem, LPDLITEM lpdli);

//==== DlGetItemEx() ==========================================================
int DirList_GetItemEx(HWND hwnd, int iItem, LPWIN32_FIND_DATA pfd);

//==== DlPropertyDlg() ========================================================
bool DirList_PropertyDlg(HWND hwnd, int iItem);

//==== DlGetLongPathName() ====================================================
bool DirList_GetLongPathName(HWND hwnd, LPWSTR lpszLongPath);

//==== DlSelectItem() =========================================================
bool DirList_SelectItem(HWND hwnd, LPCWSTR lpszDisplayName, LPCWSTR lpszFullPath);

//==== DlCreateFilter() and DlMatchFilter() ===================================

#define DL_FILTER_BUFSIZE 128

typedef struct DL_FILTER { //dlf
	int nCount;
	bool bExcludeFilter;
	WCHAR tFilterBuf[DL_FILTER_BUFSIZE];
	LPWSTR pFilter[DL_FILTER_BUFSIZE];
} DL_FILTER, *PDL_FILTER;

typedef const DL_FILTER * LPCDL_FILTER;

void DirList_CreateFilter(PDL_FILTER pdlf, LPCWSTR lpszFileSpec, bool bExcludeFilter);

bool DirList_MatchFilter(LPSHELLFOLDER lpsf, LPCITEMIDLIST pidl, LPCDL_FILTER pdlf);

//==== DriveBox ===============================================================

bool DriveBox_Init(HWND hwnd);
int  DriveBox_Fill(HWND hwnd);
bool DriveBox_GetSelDrive(HWND hwnd, LPWSTR lpszDrive, int nDrive, bool fNoSlash);
bool DriveBox_SelectDrive(HWND hwnd, LPCWSTR lpszPath);
bool DriveBox_PropertyDlg(HWND hwnd);

bool DriveBox_DeleteItem(HWND hwnd, LPARAM lParam);
bool DriveBox_GetDispInfo(HWND hwnd, LPARAM lParam);

//==== ItemID =================================================================

//==== IL_Next() ==============================================================
static inline LPITEMIDLIST IL_Next(LPITEMIDLIST pidl) {
	return (LPITEMIDLIST)((LPBYTE)(pidl) + pidl->mkid.cb);
}

//==== IL_Create() ============================================================
LPITEMIDLIST IL_Create(LPCITEMIDLIST pidl1, UINT cb1, LPCITEMIDLIST pidl2, UINT cb2);

//==== IL_GetSize() ===========================================================
UINT IL_GetSize(LPCITEMIDLIST pidl);

//==== IL_GetDisplayName() ====================================================
bool IL_GetDisplayName(LPSHELLFOLDER lpsf, LPCITEMIDLIST pidl, DWORD dwFlags, LPWSTR lpszDisplayName, int nDisplayName);
