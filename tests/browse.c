/*
 * browse - open a dialog that picks a folder.
 *
 * Usage: browse [item [save]] | [new] [edit] [status]
 *   item    the common item dialog (IFileOpenDialog with FOS_PICKFOLDERS),
 *           as the Eureka A4 emulator opens it (lpintes/EurekaA4Emulator,
 *           src/win/dialog.cpp, PickFolder)
 *   save    with item: IFileSaveDialog with FOS_PICKFOLDERS and a
 *           suggested name (PickFolderToCreate there)
 *   new     SHBrowseForFolderW with BIF_NEWDIALOGSTYLE (the resizable
 *           template with "Folder:")
 *   edit    BIF_EDITBOX (the folder name field)
 *   status  BIF_STATUSTEXT (old style only)
 *
 * With no option it is the old style "Browse for Folder" dialog
 * (SHBrowseForFolderW) without an edit box. The chosen path, or
 * "cancelled", goes to stdout. Pure Win32: builds with winegcc and with
 * mingw.
 */

#define COBJMACROS
#define _WIN32_WINNT 0x0600     /* IFileDialog */
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

/* CRT stdio is unreliable in a Winelib binary (see CLAUDE.md). */
static void print(const WCHAR *text)
{
    char buf[MAX_PATH * 3];
    DWORD written;
    int len;

    len = WideCharToMultiByte(CP_UTF8, 0, text, -1, buf, sizeof(buf), NULL,
                              NULL);
    if (len > 0)
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buf, len - 1, &written,
                  NULL);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\n", 1, &written, NULL);
}

/* The item dialog with the options of the emulator's PickFolder or
 * PickFolderToCreate. */
static void pick_item(BOOL save)
{
    IFileDialog *dialog = NULL;
    IShellItem *item = NULL;
    FILEOPENDIALOGOPTIONS options = 0;
    WCHAR *path = NULL;
    HRESULT hr;

    hr = CoCreateInstance(save ? &CLSID_FileSaveDialog : &CLSID_FileOpenDialog,
                          NULL, CLSCTX_INPROC_SERVER, &IID_IFileDialog,
                          (void **)&dialog);
    if (FAILED(hr))
    {
        print(L"no item dialog");
        return;
    }
    IFileDialog_GetOptions(dialog, &options);
    options |= FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (save)
        options = (options | FOS_NOREADONLYRETURN) & ~FOS_FILEMUSTEXIST;
    IFileDialog_SetOptions(dialog, options);
    IFileDialog_SetTitle(dialog, L"Choose the folder used as the disk");
    if (save)
        IFileDialog_SetFileName(dialog, L"NewDisk");
    if (SUCCEEDED(IFileDialog_Show(dialog, NULL)) &&
        SUCCEEDED(IFileDialog_GetResult(dialog, &item)) &&
        SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path)))
    {
        print(path);
        CoTaskMemFree(path);
    }
    else
        print(L"cancelled");
    if (item)
        IShellItem_Release(item);
    IFileDialog_Release(dialog);
}

int main(void)
{
    BROWSEINFOW bi = { 0 };
    WCHAR path[MAX_PATH];
    LPITEMIDLIST pidl;
    BOOL item = FALSE, save = FALSE;
    WCHAR **argv;
    int argc, i;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bi.lpszTitle = L"Choose the folder for the test files:";
    bi.ulFlags = BIF_RETURNONLYFSDIRS;
    for (i = 1; i < argc; i++)
    {
        if (!lstrcmpW(argv[i], L"item"))
            item = TRUE;
        else if (!lstrcmpW(argv[i], L"save"))
            save = TRUE;
        else if (!lstrcmpW(argv[i], L"new"))
            bi.ulFlags |= BIF_NEWDIALOGSTYLE;
        else if (!lstrcmpW(argv[i], L"edit"))
            bi.ulFlags |= BIF_EDITBOX;
        else if (!lstrcmpW(argv[i], L"status"))
            bi.ulFlags |= BIF_STATUSTEXT;
    }
    CoInitialize(NULL);
    if (item)
        pick_item(save);
    else
    {
        pidl = SHBrowseForFolderW(&bi);
        if (pidl && SHGetPathFromIDListW(pidl, path))
            print(path);
        else
            print(L"cancelled");
        if (pidl)
            CoTaskMemFree((void *)pidl);
    }
    CoUninitialize();
    return 0;
}
