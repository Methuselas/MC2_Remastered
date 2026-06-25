//------------------------------------------------------------------------------
// BeautySidecarPreview — EDITOR-SIDECAR-PREVIEW-1 (B7a). See header.
//------------------------------------------------------------------------------
#include "BeautySidecarPreview.h"

#include "EditorData.h"
#include "terrain.h"
#include "mapdata.h"
#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>

namespace BeautySidecarPreview
{
	static std::vector<float> s_origElev;   // pre-apply snapshot (full grid)
	static int  s_side    = 0;
	static int  s_changed = 0;
	static bool s_applied = false;
	static bool s_autoApplyDone = false;
	static char s_status[256] = "no sidecar loaded";

	// Locate <mission>.beauty/height_delta.r32 in a few candidate dirs (editor
	// CWD = deploy root). Returns true + fills delta/side on success.
	static bool readDelta(const char* mission, std::vector<float>& delta, int& side)
	{
		if (!mission || !mission[0]) { strcpy(s_status, "no mission loaded"); return false; }
		const char* fmts[] = {
			"data/missions/%s.beauty/height_delta.r32",
			"%s.beauty/height_delta.r32",
			"tests/terrain/beautify/%s.beauty/height_delta.r32",
		};
		char path[512];
		FILE* fp = nullptr;
		for (int i = 0; i < 3 && !fp; ++i) {
			snprintf(path, sizeof(path), fmts[i], mission);
			fp = fopen(path, "rb");
		}
		if (!fp) {
			snprintf(s_status, sizeof(s_status), "no sidecar for '%s' (looked in data/missions, cwd, tests)", mission);
			return false;
		}
		fseek(fp, 0, SEEK_END);
		long bytes = ftell(fp);
		fseek(fp, 0, SEEK_SET);
		long n = bytes / 4;
		int s = (int)(sqrt((double)n) + 0.5);
		if (s * s != n || n <= 0) {
			fclose(fp);
			snprintf(s_status, sizeof(s_status), "sidecar not square (%ld floats)", n);
			return false;
		}
		delta.resize((size_t)n);
		size_t got = fread(delta.data(), sizeof(float), (size_t)n, fp);
		fclose(fp);
		if (got != (size_t)n) { strcpy(s_status, "sidecar read short"); return false; }
		side = s;
		return true;
	}

	bool Apply()
	{
		if (!land || !Terrain::mapData) { strcpy(s_status, "no terrain loaded"); return false; }
		const char* mission = EditorData::instance ? EditorData::instance->MissionName().Data() : "";

		std::vector<float> delta;
		int dside = 0;
		if (!readDelta(mission, delta, dside)) return false;   // status set inside

		const int side = land->realVerticesMapSide;
		if (dside != side) {
			snprintf(s_status, sizeof(s_status), "grid mismatch: sidecar=%d terrain=%d", dside, side);
			return false;
		}
		const int total = side * side;
		PostcompVertexPtr blocks = Terrain::mapData->getBlocks();
		if (!blocks) { strcpy(s_status, "terrain blocks null"); return false; }

		// Snapshot once (so repeated Apply/Restore stay anchored to stock).
		if (!s_applied) {
			s_origElev.resize((size_t)total);
			for (int i = 0; i < total; ++i) s_origElev[(size_t)i] = blocks[i].elevation;
			s_side = side;
		}

		int changed = 0;
		for (int i = 0; i < total; ++i) {
			if (delta[(size_t)i] != 0.0f) {
				land->setVertexHeight(i, s_origElev[(size_t)i] + delta[(size_t)i]);
				++changed;
			}
		}
		Terrain::mapData->calcLight();          // recompute per-vertex normals
		EditorData::refreshTerrainAfterEdit();  // rebuild face cache + GPU recipes

		s_applied = true;
		s_changed = changed;
		snprintf(s_status, sizeof(s_status), "applied %d cells (%s)", changed, mission);
		return true;
	}

	void Restore()
	{
		if (!s_applied || !land || s_side <= 0) return;
		const int total = s_side * s_side;
		for (int i = 0; i < total; ++i) land->setVertexHeight(i, s_origElev[(size_t)i]);
		Terrain::mapData->calcLight();
		EditorData::refreshTerrainAfterEdit();
		s_applied = false;
		strcpy(s_status, "restored original terrain");
	}

	bool        IsApplied()    { return s_applied; }
	int         ChangedCount() { return s_changed; }
	const char* Status()       { return s_status; }

	void DrawImGui()
	{
		ImGui::SeparatorText("Beauty Sidecar Preview");
		if (ImGui::Button(s_applied ? "Re-apply Beauty Sidecar" : "Apply Beauty Sidecar", ImVec2(-1.f, 0.f)))
			Apply();
		ImGui::BeginDisabled(!s_applied);
		if (ImGui::Button("Restore Original Terrain", ImVec2(-1.f, 0.f)))
			Restore();
		ImGui::EndDisabled();
		ImGui::TextWrapped("%s", s_status);
	}

	void MaybeAutoApply()
	{
		if (s_autoApplyDone) return;
		const char* e = getenv("MC2_EDITOR_BEAUTY_AUTOAPPLY");
		if (!e || !e[0] || e[0] == '0') { s_autoApplyDone = true; return; }
		if (!land || !Terrain::mapData) return;  // wait until terrain is loaded
		s_autoApplyDone = true;
		bool ok = Apply();
		fprintf(stderr, "[EDITOR_BEAUTY_AUTOAPPLY] %s (%s)\n", ok ? "OK" : "FAIL", s_status);
	}
}
