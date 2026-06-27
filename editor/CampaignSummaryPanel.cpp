/***************************************************************
* FILENAME: CampaignSummaryPanel.cpp
* DESCRIPTION: Read-only campaign explainer. See CampaignSummaryPanel.h.
*   The editor keeps no always-loaded campaign object, so this panel loads a
*   campaign .fit on demand via the same Win32 GetOpenFileNameA idiom the Map
*   Generator import uses, reads it into a file-static CCampaignData, and renders
*   the operation/mission outline with cheap data-only validation badges.
*
*   CCampaignData::Read() (CampaignData.cpp:225) may assert() on a malformed
*   file; that is acceptable in RelWithDebInfo per the slice contract -- we do
*   not add engine guards.
*
*   PURE-ADDITIVE, READ-ONLY: no save, no editing.
* DATE: 2026-06-27
****************************************************************/

// Pull stdafx first (MFC / Windows headers, incl. commdlg GetOpenFileNameA).
#include "stdafx.h"

#ifdef MC2_IMGUI

#include "imgui.h"

#include "CampaignSummaryPanel.h"

#include "campaignData.h"   // CCampaignData / CGroupData / CMissionData

#include <cstdio>
#include <cstring>
#include <cstdarg>

// ---------------------------------------------------------------------------
// Panel state
// ---------------------------------------------------------------------------
static bool          s_open = false;
static bool          s_loaded = false;       // a campaign has been read at least once
static CCampaignData s_campaign;             // cached, read-only display copy
static char          s_loadedPath[1024] = "";
static char          s_status[256] = "";

void CampaignSummaryPanel::Open()   { s_open = true; }
void CampaignSummaryPanel::Close()  { s_open = false; }
void CampaignSummaryPanel::Toggle() { s_open = !s_open; }
bool CampaignSummaryPanel::IsOpen() { return s_open; }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Win32 file picker -> dst. Returns true if the user picked a file. Mirrors the
// BrowseForFile idiom in MapGeneratorDialog.cpp.
static bool campBrowseForFile(const char* filter, char* dst, size_t dstLen)
{
    char path[1024] = "";
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = ::GetActiveWindow();
    ofn.lpstrFilter = filter;
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = sizeof(path);
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&ofn))
        return false;
    strncpy(dst, path, dstLen - 1);
    dst[dstLen - 1] = '\0';
    return true;
}

// Colored "[warning] ..." line (data-only validation badge).
static void campWarn(const char* fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.20f, 1.0f));
    ImGui::BulletText("[warning] %s", buf);
    ImGui::PopStyleColor();
}

// CString -> const char* for display (MFC CString has operator LPCTSTR()).
static const char* campStr(const CString& s) { return (const char*)s; }
static bool        campEmpty(const CString& s) { return s.GetLength() == 0; }
static bool        campIs(const CString& s, const char* lit) { return std::strcmp((const char*)s, lit) == 0; }

// ---------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------
void CampaignSummaryPanel::Draw()
{
    if (!s_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(460.f, 520.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Campaign Overview", &s_open))
    {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("Read-only campaign overview. Edit via the legacy Campaign dialog.");
    ImGui::Separator();

    if (ImGui::Button("Load campaign .fit..."))
    {
        char path[1024] = "";
        if (campBrowseForFile("Campaign (*.fit)\0*.fit\0All\0*.*\0", path, sizeof(path)))
        {
            // Fresh copy each load (Read appends groups onto m_GroupList).
            CCampaignData fresh;
            if (fresh.Read(path))
            {
                s_campaign = fresh;
                s_loaded = true;
                strncpy(s_loadedPath, path, sizeof(s_loadedPath) - 1);
                s_loadedPath[sizeof(s_loadedPath) - 1] = '\0';
                std::snprintf(s_status, sizeof(s_status), "Loaded: %s", s_loadedPath);
            }
            else
            {
                std::snprintf(s_status, sizeof(s_status), "Failed to read: %s", path);
            }
        }
    }
    if (s_status[0])
        ImGui::TextDisabled("%s", s_status);

    ImGui::Separator();

    if (!s_loaded)
    {
        ImGui::TextDisabled("No campaign loaded. Use the button above.");
        ImGui::End();
        return;
    }

    // -- Campaign header ----------------------------------------------------
    if (s_campaign.m_NameUseResourceString)
        ImGui::Text("Campaign: (resource string #%d)", s_campaign.m_NameResourceStringID);
    else
        ImGui::Text("Campaign: %s",
                    campEmpty(s_campaign.m_Name) ? "(unnamed)" : campStr(s_campaign.m_Name));
    ImGui::Text("Starting C-Bills: %d", s_campaign.m_CBills);
    ImGui::Text("Final video: %s",
                campEmpty(s_campaign.m_FinalVideo) ? "(none)" : campStr(s_campaign.m_FinalVideo));

    const unsigned long opCount = s_campaign.m_GroupList.Count();
    ImGui::Text("Operations: %lu", opCount);

    // Campaign-level validation.
    if (opCount == 0)
        campWarn("Campaign has 0 operations.");

    ImGui::Separator();

    // -- Operations ---------------------------------------------------------
    int opIndex = 0;
    for (CGroupList::EIterator git = s_campaign.m_GroupList.Begin(); !git.IsDone(); git++, ++opIndex)
    {
        CGroupData& grp = *git;
        const unsigned long missionCount = grp.m_MissionList.Count();

        char opHdr[160];
        std::snprintf(opHdr, sizeof(opHdr), "Operation %d: %s###op%d",
                      opIndex,
                      campEmpty(grp.m_Label) ? "(no label)" : campStr(grp.m_Label),
                      opIndex);

        if (!ImGui::CollapsingHeader(opHdr, ImGuiTreeNodeFlags_DefaultOpen))
            continue;

        ImGui::Indent();

        // Progression, in plain English.
        ImGui::Text("Complete %d of %lu missions to advance",
                    grp.m_NumMissionsToComplete, missionCount);
        if (missionCount > 0)
        {
            if ((unsigned long)grp.m_NumMissionsToComplete == missionCount)
                ImGui::TextDisabled("  (all required)");
            else if ((unsigned long)grp.m_NumMissionsToComplete < missionCount)
                ImGui::TextDisabled("  (may skip %lu)",
                                    missionCount - (unsigned long)grp.m_NumMissionsToComplete);
        }

        ImGui::Text("Operation file: %s",
                    campEmpty(grp.m_OperationFile) ? "(none)" : campStr(grp.m_OperationFile));
        ImGui::Text("Briefing: %s",
                    campEmpty(grp.m_PreVideoFile) ? "(none)" : campStr(grp.m_PreVideoFile));
        ImGui::Text("Video: %s",
                    campEmpty(grp.m_VideoFile) ? "(none)" : campStr(grp.m_VideoFile));
        ImGui::Text("Music tune: %d", grp.m_TuneNumber);

        // Missions.
        if (missionCount == 0)
        {
            ImGui::TextDisabled("Missions: (none)");
        }
        else
        {
            ImGui::Text("Missions:");
            for (CMissionList::EIterator mit = grp.m_MissionList.Begin(); !mit.IsDone(); mit++)
            {
                CMissionData& m = *mit;
                ImGui::BulletText("%s",
                    campEmpty(m.m_MissionFile) ? "(empty filename)" : campStr(m.m_MissionFile));
            }
        }

        // Per-operation validation badges (data-only, cheap).
        if (missionCount == 0)
            campWarn("Operation has 0 missions.");
        if ((unsigned long)grp.m_NumMissionsToComplete > missionCount)
            campWarn("NumMissionsToComplete (%d) exceeds mission count (%lu).",
                     grp.m_NumMissionsToComplete, missionCount);
        if (campEmpty(grp.m_Label))
            campWarn("Operation label is empty.");
        if (campIs(grp.m_VideoFile, "STANDIN"))
            campWarn("Video is a STANDIN placeholder.");
        if (campIs(grp.m_PreVideoFile, "STANDIN"))
            campWarn("Briefing video is a STANDIN placeholder.");
        for (CMissionList::EIterator mit = grp.m_MissionList.Begin(); !mit.IsDone(); mit++)
        {
            if (campEmpty((*mit).m_MissionFile))
            {
                campWarn("An operation mission has an empty filename.");
                break;
            }
        }

        ImGui::Unindent();
    }

    ImGui::End();
}

#endif // MC2_IMGUI
