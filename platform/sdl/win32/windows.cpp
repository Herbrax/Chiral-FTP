// Windows: the folder chooser, the system's own (IFileOpenDialog), for the
// server screen's "Choose..." -- what macos.mm is to the Mac.

#include <windows.h>
#include <shobjidl.h>

#include <string>

bool windows_choose_folder(const char *start, std::string &out) {
  bool chosen = false;
  const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  IFileOpenDialog *dialog = nullptr;

  if(FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
    if(SUCCEEDED(init)) CoUninitialize();
    return false;
  }
  DWORD options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);

  wchar_t w[PATH_MAX + 8];
  IShellItem *folder = nullptr;
  if(start && chiral_win_path(start, w, sizeof(w) / sizeof(w[0])) > 0 &&
     SUCCEEDED(SHCreateItemFromParsingName(w, nullptr, IID_PPV_ARGS(&folder)))) {
    dialog->SetFolder(folder);
    folder->Release();
  }

  IShellItem *item = nullptr;
  if(SUCCEEDED(dialog->Show(GetActiveWindow())) && SUCCEEDED(dialog->GetResult(&item))) {
    PWSTR path = nullptr;
    if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
      char posix[PATH_MAX];
      chiral_posix_path(path, posix, sizeof(posix));
      out = posix;
      chosen = true;
      CoTaskMemFree(path);
    }
    item->Release();
  }
  dialog->Release();
  if(SUCCEEDED(init)) CoUninitialize();
  return chosen;
}
