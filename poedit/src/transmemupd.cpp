
/*

    poedit, a wxWindows i18n catalogs editor

    ---------------
      transmemupd.cpp
    
      Translation memory database updater
    
      (c) Vaclav Slavik, 2001

*/


#ifdef __GNUG__
#pragma implementation
#endif

#include <wx/wxprec.h>

#ifdef USE_TRANSMEM

#include <wx/string.h>
#include <wx/tokenzr.h>
#include <wx/log.h>
#include <wx/intl.h>
#include <wx/utils.h>
#include <wx/dir.h>

#include "catalog.h"
#include "transmem.h"
#include "transmemupd.h"
#include "gexecute.h"
#include "progressinfo.h"

/** Does given file contain catalogs in given language?
    Handles these cases:
      - foo/bar/lang.mo
      - foo/lang/bar.mo
      - foo/lang/LC_MESSAGES/bar.mo
    Futhermore, if \a lang is 2-letter code (e.g. "cs"), handles these:
      - foo/bar/lang_??.mo
      - foo/lang_??/bar.mo
      - foo/lang_??/LC_MESSAGES/bar.mo
    and if \a lang is five-letter code (e.g. "cs_CZ"), tries to match its
    first two letters (i.e. country-neutral variant of the language).
 */
static inline bool IsForLang(const wxString& filename, const wxString& lang,
                             bool variants = true)
{
#ifdef __WINDOWS__
    #define LC_MESSAGES_STR "/lc_messages"
#else
    #define LC_MESSAGES_STR "/LC_MESSAGES"
#endif
    wxString base, dir;
    wxSplitPath(filename, &dir, &base, NULL);
    dir.Replace(wxString(wxFILE_SEP_PATH), _T("/"));
    return base.Matches(lang) ||
           dir.Matches(_T("*/") + lang) ||
           dir.Matches(_T("*/") + lang + LC_MESSAGES_STR) ||
           (variants && lang.Len() == 5 && lang[2] == _T('_') && 
                IsForLang(filename, lang.Mid(0, 2), false)) ||
           (variants && lang.Len() == 2 && 
                IsForLang(filename, lang + _T("_??"), false));
    #undef LC_MESSAGES_STR
}

class TMUDirTraverser : public wxDirTraverser
{
    public:
        TMUDirTraverser(wxArrayString *files, const wxString& lang)
            : m_files(files), m_lang(lang) {}
        virtual ~TMUDirTraverser() {}
            
        virtual wxDirTraverseResult OnFile(const wxString& filename)
        {
#ifdef __WINDOWS__
            wxString f = filename.Lower();
#else
            wxString f = filename;
#endif
            if ((f.Matches(_T("*.mo")) && IsForLang(f, m_lang)) || 
                (f.Matches(_T("*.po")) && IsForLang(f, m_lang)) 
#ifdef __UNIX__
                || f.Matches(_T("*.rpm"))
#endif
                )
            {
                m_files->Add(f);
            }
            return wxDIR_CONTINUE;
        }
        
        virtual wxDirTraverseResult OnDir(const wxString& dirname)
            { return wxDIR_CONTINUE; }
        
    private:
        wxArrayString *m_files;
        wxString m_lang;
};

TranslationMemoryUpdater::TranslationMemoryUpdater(TranslationMemory *mem, 
                                                   ProgressInfo *pi)
    : m_progress(pi), m_mem(mem)
{
}

bool TranslationMemoryUpdater::Update(const wxArrayString& paths)
{
    size_t i;
    wxString f;
    wxArrayString files;
    TMUDirTraverser trav(&files, m_mem->GetLanguage());

    m_progress->UpdateMessage(_("Listing files..."));
    for (i = 0; i < paths.GetCount(); i++)
    {
        wxDir dir(paths[i]);
        if (!dir.IsOpened() || dir.Traverse(trav) == (size_t)-1) 
            return false;
    }

    m_progress->SetGaugeMax(files.GetCount());
    m_progress->ResetGauge();

    bool res = true;
    size_t cnt = files.GetCount();
    for (i = 0; i < cnt; i++)
    {
        f = files[i];
        m_progress->UpdateMessage(wxString(_("Scanning file: ")) + 
                                  wxFileNameFromPath(f));
        
        if (f.Matches(_T("*.po")))
            res = UpdateFromPO(f);
        else if (f.Matches(_T("*.mo")))
            res = UpdateFromMO(f);
#ifdef __UNIX__
        else if (f.Matches(_T("*.rpm")))
            res = UpdateFromRPM(f);
#endif
            
        m_progress->UpdateGauge();
        wxYield();
        if (m_progress->Cancelled()) return false;
    }
    return res;
}

bool TranslationMemoryUpdater::UpdateFromPO(const wxString& filename)
{
    return UpdateFromCatalog(filename);
}

bool TranslationMemoryUpdater::UpdateFromMO(const wxString& filename)
{
    wxString tmp;
    wxLogNull null;

    if (!wxGetTempFileName(_T("poedit"), tmp))
        return false;
    if (!ExecuteGettext(_T("msgunfmt --force-po -o ") + tmp + _T(" ") + filename))
        return false;
    bool rt = UpdateFromCatalog(tmp);
    wxRemoveFile(tmp);
    return rt;
}

#ifdef __UNIX__
bool TranslationMemoryUpdater::UpdateFromRPM(const wxString& filename)
{
    #define TMP_DIR _T("/tmp/poedit-rpm-tpm")
    if (!wxMkdir(TMP_DIR)) return false;
    
    wxString cmd;
    cmd.Printf(_T("sh -c '(cd %s ; rpm2cpio %s | cpio -i -d --quiet \"*.mo\")'"),
               TMP_DIR, filename.c_str());
    if (wxExecute(cmd, true) != 0)
    {
        wxLogError(_("Cannot extract catalogs from RPM file."));
        wxExecute(_T("rm -rf ") TMP_DIR, true);
        return false;
    }

    bool res = true;
    wxArrayString files;
    if (wxDir::GetAllFiles(TMP_DIR, &files, _T("*.mo")) != (size_t)-1)
    {
        size_t cnt = files.GetCount();
        for (size_t i = 0; res && i < cnt; i++)
        {
            if (!IsForLang(files[i], m_mem->GetLanguage())) continue;
            if (!UpdateFromMO(files[i])) 
                res = false;
        }
    }

    wxLog::FlushActive();
    wxExecute(_T("rm -rf ") TMP_DIR, true);
    return res;
    #undef TMP_DIR
}
#endif

bool TranslationMemoryUpdater::UpdateFromCatalog(const wxString& filename)
{
    wxLogNull null;
    Catalog cat(filename);
    
    if (!cat.IsOk()) return true; // ignore

    size_t cnt = cat.GetCount();
    for (size_t i = 0; i < cnt; i++)
    {
        CatalogData &dt = cat[i];
        if (!dt.IsTranslated() || dt.IsFuzzy()) continue;
        if (!m_mem->Store(dt.GetString(), dt.GetTranslation())) 
            return false;
    }
    return true;
}

#endif //USE_TRANSMEM