#include "stdafx.h"
#include "ObjectiveTimerWindow.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <GWCA\Constants\Constants.h>
#include <GWCA\GameContainers\Array.h>
#include <GWCA\GameContainers\GamePos.h>
#include <GWCA\Packets\StoC.h>

#include <GWCA\GameEntities\Map.h>
#include <GWCA\GameEntities\Agent.h>

#include <GWCA\Context\GameContext.h>
#include <GWCA\Context\WorldContext.h>

#include <GWCA\Managers\UIMgr.h>
#include <GWCA\Managers\MapMgr.h>
#include <GWCA\Managers\ChatMgr.h>
#include <GWCA\Managers\StoCMgr.h>
#include <GWCA\Managers\AgentMgr.h>

#include "GuiUtils.h"
#include "GWToolbox.h"

#include <Modules\Resources.h>
#include "logger.h"


#define countof(arr) (sizeof(arr) / sizeof(arr[0]))

#define TIME_UNKNOWN -1
unsigned int ObjectiveTimerWindow::ObjectiveSet::cur_ui_id = 0;

namespace {
    // settings in the unnamed namespace. This is ugly. DO NOT COPY. 
    // just doing it because Objective needs them and I'm too lazy to pass them all the way there.
    int n_columns = 4;
    bool show_decimal = false;
    bool show_start_column = true;
    bool show_end_column = true;
    bool show_time_column = true;

    enum DoA_ObjId : DWORD {
        Foundry = 0x273F,
        Veil,
        Gloom,
        City
    };
    uint32_t doa_get_next(uint32_t id) {
        switch (id) {
        case Foundry: return City;
        case City: return Veil;
        case Veil: return Gloom;
        case Gloom: return Foundry;
        }
        return 0;
    }

    void PrintTime(char* buf, size_t size, DWORD time, bool show_ms = true) {
        if (time == TIME_UNKNOWN) {
            GuiUtils::StrCopy(buf, "--:--", size);
        } else {
            DWORD sec = time / 1000;
            if (show_ms && show_decimal) {
                snprintf(buf, size, "%02d:%02d.%1d",
                    (sec / 60), sec % 60, (time / 100) % 10);
            } else {
                snprintf(buf, size, "%02d:%02d", (sec / 60), sec % 60);
            }
        }
    }

	void AsyncGetMapName(char *buffer, size_t n) {
		static wchar_t enc_str[16];
		GW::AreaInfo *info = GW::Map::GetCurrentMapInfo();
		if (!GW::UI::UInt32ToEncStr(info->name_id, enc_str, n)) {
			buffer[0] = 0;
			return;
		}
		GW::UI::AsyncDecodeStr(enc_str, buffer, n);
	}

    void ComputeNColumns() {
        n_columns = 0
            + (show_start_column ? 1 : 0)
            + (show_end_column ? 1 : 0)
            + (show_time_column ? 1 : 0);
    }
    
    float GetTimestampWidth() {
        return (75.0f * ImGui::GetIO().FontGlobalScale);
    }
    float GetLabelWidth() {
        return ImGui::GetWindowContentRegionWidth() - ImGui::GetStyle().WindowPadding.x - ((GetTimestampWidth() + ImGui::GetStyle().ItemInnerSpacing.x) * n_columns);
    }
}

void ObjectiveTimerWindow::Initialize() {
    ToolboxWindow::Initialize();

    GW::StoC::AddCallback<GW::Packet::StoC::MessageServer>(
        [this](GW::Packet::StoC::MessageServer* packet) -> bool {
            if (GW::Map::GetMapID() != GW::Constants::MapID::Urgozs_Warren)
                return false; // Only care about Urgoz
            GW::Array<wchar_t>* buff = &GW::GameContext::instance()->world->message_buff;
            if (!buff || !buff->valid() || !buff->size())
                return true; // Message buffer empty!?
            const wchar_t* msg = buff->begin();
            if (msg[0] != 0x6C9C || (msg[5] != 0x2810 && msg[5] != 0x1488))
                return false;
            // Gained 10,000 Kurzick faction in Urgoz Warren - get Urgoz objective.
            Objective* obj = GetCurrentObjective(15529);
            if (!obj || obj->IsDone())
                return false; // Already done!?
            obj->SetDone();
            ObjectiveSet* os = objective_sets.back();
            for (Objective& objective : os->objectives) {
                objective.SetDone();
            }
            os->active = false;
            return false;
        });

    GW::StoC::AddCallback<GW::Packet::StoC::PartyDefeated>(
        [this](GW::Packet::StoC::PartyDefeated* packet) -> bool {
            if (!objective_sets.empty()) {
                ObjectiveSet* os = objective_sets.back();
                os->StopObjectives();
            }
            return false;
        });

    GW::StoC::AddCallback<GW::Packet::StoC::GameSrvTransfer>(
        [this](GW::Packet::StoC::GameSrvTransfer *packet) -> bool {
        if (!objective_sets.empty()) {
            ObjectiveSet *os = objective_sets.back();
            os->StopObjectives();
        }
        return false;
    });
    GW::StoC::AddCallback<GW::Packet::StoC::InstanceLoadFile>(
        [this](GW::Packet::StoC::InstanceLoadFile* packet) -> bool {
            if (packet->map_fileID == 219215)
                AddDoAObjectiveSet(packet->spawn_point);
            return false;
        });
    GW::StoC::AddCallback<GW::Packet::StoC::InstanceLoadInfo>(
        [this](GW::Packet::StoC::InstanceLoadInfo* packet) -> bool {
            if (!packet->is_explorable)
                return false;
            switch (static_cast<GW::Constants::MapID>(packet->map_id)) {
                case GW::Constants::MapID::Urgozs_Warren: 
                    AddUrgozObjectiveSet(); break;
                case GW::Constants::MapID::The_Deep: 
                    AddDeepObjectiveSet(); break;
                case GW::Constants::MapID::The_Fissure_of_Woe:
                    AddFoWObjectiveSet(); break;
                case GW::Constants::MapID::The_Underworld:
                    AddUWObjectiveSet(); break;
            }
            return false;
        });

    GW::StoC::AddCallback<GW::Packet::StoC::ManipulateMapObject>(
        [this](GW::Packet::StoC::ManipulateMapObject* packet) -> bool {
            if (packet->animation_type != 16 || GW::Map::GetInstanceType() != GW::Constants::InstanceType::Explorable)
                return false; // Door not open or not in explorable area
            if (GW::Map::GetMapID() != GW::Constants::MapID::Urgozs_Warren)
                return false; // Urgoz only
            // For Urgoz, the id of the objective is actually the door object_id
            Objective* obj = GetCurrentObjective(packet->object_id);
            if (!obj || obj->IsStarted())
                return false;
            ObjectiveSet* os = objective_sets.back();
            obj->SetStarted();
            for (Objective& objective : os->objectives) {
                if (objective.id == packet->object_id)
                    break;
                objective.SetDone();
            }
            return false;
        });

	GW::StoC::AddCallback<GW::Packet::StoC::ObjectiveUpdateName>(
	[this](GW::Packet::StoC::ObjectiveUpdateName* packet) -> bool {
		Objective *obj = GetCurrentObjective(packet->objective_id);
        if (obj) obj->SetStarted();
        return false;
	});
	
	GW::StoC::AddCallback<GW::Packet::StoC::ObjectiveDone>(
	[this](GW::Packet::StoC::ObjectiveDone* packet) -> bool {
        Objective* obj = GetCurrentObjective(packet->objective_id);
        if (obj) {
            obj->SetDone();
            objective_sets.back()->CheckSetDone();
        }
        return false;
	});

    GW::StoC::AddCallback<GW::Packet::StoC::AgentUpdateAllegiance>(
        [this](GW::Packet::StoC::AgentUpdateAllegiance* packet) -> bool {
        if (GW::Map::GetMapID() != GW::Constants::MapID::The_Underworld) return false;

        const GW::Agent* agent = GW::Agents::GetAgentByID(packet->agent_id);
        if (agent == nullptr) return false;
        if (agent->player_number != GW::Constants::ModelID::UW::Dhuum) return false;
        if (packet->unk1 != 0x6D6F6E31) return false;
        
        Objective* obj = GetCurrentObjective(157);
        if (obj && !obj->IsStarted()) obj->SetStarted();
        return false;
    });

	GW::StoC::AddCallback<GW::Packet::StoC::DoACompleteZone>(
	[this](GW::Packet::StoC::DoACompleteZone* packet) -> bool {
		if (packet->message[0] != 0x8101) return false;
		if (objective_sets.empty()) return false;

		uint32_t id = packet->message[1];
		Objective *obj = GetCurrentObjective(id);
		ObjectiveSet *os = objective_sets.back();

        if (obj) {
            obj->SetDone();
            os->CheckSetDone();
            uint32_t next_id = doa_get_next(id);
            Objective *next = GetCurrentObjective(next_id);
            if (next && !next->IsStarted()) next->SetStarted();
        }
		return false;
	});
}

void ObjectiveTimerWindow::ObjectiveSet::StopObjectives() {
    active = false;
	for (Objective& obj : objectives) {
        if (obj.status == Objective::Started) {
            obj.status = Objective::Failed;
        }
	}
}

void ObjectiveTimerWindow::AddDoAObjectiveSet(GW::Vec2f spawn) {
    static const GW::Vec2f area_spawns[] = {
        { -10514, 15231 },  // foundry
        { -18575, -8833 },  // city
        { 364, -10445 },    // veil
        { 16034, 1244 },    // gloom
    };
    const GW::Vec2f mallyx_spawn(-3931, -6214);

    const int n_areas = 4;
    double best_dist = GW::GetDistance(spawn, mallyx_spawn);
    int area = -1;
    for (int i = 0; i < n_areas; ++i) {
        float dist = GW::GetDistance(spawn, area_spawns[i]);
        if (best_dist > dist) {
            best_dist = dist;
            area = i;
        }
    }
    if (area == -1) return; // we're doing mallyx, not doa!

	ObjectiveSet *os = new ObjectiveSet;
	::AsyncGetMapName(os->name, sizeof(os->name));
    Objective objs[n_areas] = {{Foundry, "Foundry"}, {City, "City"}, {Veil, "Veil"}, {Gloom, "Gloom"}};

    for (int i = 0; i < n_areas; ++i) {
        os->objectives.push_back(objs[(area + i) % n_areas]);
    }

    os->objectives.front().SetStarted();
    objective_sets.push_back(os);
}
void ObjectiveTimerWindow::AddUrgozObjectiveSet() {
    // Zone 1, Weakness = already open on start
    // Zone 2, Life Drain = Starts when door 45420 opens
    // Zone 3, Levers = Starts when door 11692 opens
    // Zone 4, Bridge = Starts when door 54552 opens
    // Zone 5, Wolves = Starts when door 1760 opens
    // Zone 6, Energy Drain = Starts when door 40330 opens
    // Zone 7, Exhaustion = Starts when door 29537 opens 60114? 54756?
    // Zone 8, Pillars = Starts when door 37191 opens
    // Zone 9, Blood Drinkers = Starts when door 35500 opens
    // Zone 10, Jons Fail Room = Starts when door 34278 opens
    // Zone 11, Urgoz = Starts when door 15529 opens
    // Urgoz = 3750
    // Objective for Urgoz = 357

    ObjectiveTimerWindow::ObjectiveSet* os = new ObjectiveSet;
    ::AsyncGetMapName(os->name, sizeof(os->name));
    os->objectives.emplace_back(1, "Zone 1 | Weakness");
    os->objectives.emplace_back(45420, "Zone 2 | Life Drain");
    os->objectives.emplace_back(11692, "Zone 3 | Levers");
    os->objectives.emplace_back(54552, "Zone 4 | Bridge Wolves");
    os->objectives.emplace_back(1760, "Zone 5 | More Wolves");
    os->objectives.emplace_back(40330, "Zone 6 | Energy Drain");
    os->objectives.emplace_back(29537, "Zone 7 | Exhaustion");
    os->objectives.emplace_back(37191, "Zone 8 | Pillars");
    os->objectives.emplace_back(35500, "Zone 9 | Blood Drinkers");
    os->objectives.emplace_back(34278, "Zone 10 | Bridge");
    os->objectives.emplace_back(15529, "Zone 11 | Urgoz");
    // 45631 53071 are the object_ids for the left and right urgoz doors
    os->objectives.front().SetStarted();
    objective_sets.push_back(os);
}
void ObjectiveTimerWindow::AddDeepObjectiveSet() {
    // Room 1 = 1760 + 54552
    // Room 2 = 45425 + 48290
    // Room 3 = 11692 + 12669
    // Room 4 = 29594 + 40330
    // Room 5 = 49742
}
void ObjectiveTimerWindow::AddFoWObjectiveSet() {
	ObjectiveSet *os = new ObjectiveSet;
	::AsyncGetMapName(os->name, sizeof(os->name));
	os->objectives.emplace_back(309, "ToC");
	os->objectives.emplace_back(310, "Wailing Lord");
	os->objectives.emplace_back(311, "Griffons");
	os->objectives.emplace_back(312, "Defend");
	os->objectives.emplace_back(313, "Forge");
	os->objectives.emplace_back(314, "Menzies");
	os->objectives.emplace_back(315, "Restore");
	os->objectives.emplace_back(316, "Khobay");
	os->objectives.emplace_back(317, "ToS");
	os->objectives.emplace_back(318, "Burning Forest");
	os->objectives.emplace_back(319, "The Hunt");
	objective_sets.push_back(os);
}
void ObjectiveTimerWindow::AddUWObjectiveSet() {
	ObjectiveSet *os = new ObjectiveSet;
	::AsyncGetMapName(os->name, sizeof(os->name));
	os->objectives.emplace_back(146, "Chamber");
	os->objectives.emplace_back(147, "Restore");
	os->objectives.emplace_back(148, "Escort");
	os->objectives.emplace_back(149, "UWG");
	os->objectives.emplace_back(150, "Vale");
	os->objectives.emplace_back(151, "Waste");
	os->objectives.emplace_back(152, "Pits");
	os->objectives.emplace_back(153, "Planes");
	os->objectives.emplace_back(154, "Mnts");
	os->objectives.emplace_back(155, "Pools");
	os->objectives.emplace_back(157, "Dhuum");
	objective_sets.push_back(os);
}

void ObjectiveTimerWindow::Update(float delta) {
    if (!objective_sets.empty() && objective_sets.back()->active) {
        objective_sets.back()->Update();
    }
}
void ObjectiveTimerWindow::Draw(IDirect3DDevice9* pDevice) {
	if (!visible) return;

	ImGui::SetNextWindowPosCenter(ImGuiSetCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiSetCond_FirstUseEver);
    if (!ImGui::Begin(Name(), GetVisiblePtr(), GetWinFlags()))
        return ImGui::End();
    if (objective_sets.empty()) {
        ImGui::Text("Enter DoA, FoW, UW or Urgoz to begin");
        return ImGui::End();
    }
    for (auto& it = objective_sets.rbegin(); it != objective_sets.rend(); it++) {
        bool show = (*it)->Draw();
        if (!show) {
            objective_sets.erase(--(it.base()));
            break;
            // iterators go crazy, don't even bother, we're skipping a frame. NBD.
            // if you really want to draw the rest make sure you extensively test this.
        }
    }
	ImGui::End();
}

ObjectiveTimerWindow::Objective* ObjectiveTimerWindow::GetCurrentObjective(uint32_t obj_id) {
    if (objective_sets.empty()) return nullptr;
    if (!objective_sets.back()->active) return nullptr;

    for (Objective& objective : objective_sets.back()->objectives) {
        if (objective.id == obj_id) {
            return &objective;
        }
    }
    return nullptr;
}

void ObjectiveTimerWindow::DrawSettingInternal() {
    ImGui::Checkbox("Show second decimal", &show_decimal);
    ImGui::Checkbox("Show 'Start' column", &show_start_column);
    ImGui::Checkbox("Show 'End' column", &show_end_column);
    ImGui::Checkbox("Show 'Time' column", &show_time_column);
    ComputeNColumns();
}

void ObjectiveTimerWindow::LoadSettings(CSimpleIni* ini) {
	ToolboxWindow::LoadSettings(ini);
    show_decimal = ini->GetBoolValue(Name(), VAR_NAME(show_decimal), show_decimal);
    show_start_column = ini->GetBoolValue(Name(), VAR_NAME(show_start_column), show_start_column);
    show_end_column = ini->GetBoolValue(Name(), VAR_NAME(show_end_column), show_end_column);
    show_time_column = ini->GetBoolValue(Name(), VAR_NAME(show_time_column), show_time_column);
    ComputeNColumns();
}

void ObjectiveTimerWindow::SaveSettings(CSimpleIni* ini) {
	ToolboxWindow::SaveSettings(ini);
    ini->SetBoolValue(Name(), VAR_NAME(show_decimal), show_decimal);
    ini->SetBoolValue(Name(), VAR_NAME(show_start_column), show_start_column);
    ini->SetBoolValue(Name(), VAR_NAME(show_end_column), show_end_column);
    ini->SetBoolValue(Name(), VAR_NAME(show_time_column), show_time_column);
}


ObjectiveTimerWindow::Objective::Objective(uint32_t _id, const char* _name) {
    id = _id;
    GuiUtils::StrCopy(name, _name, sizeof(name));
    start = TIME_UNKNOWN;
    done = TIME_UNKNOWN;
    duration = TIME_UNKNOWN;
    PrintTime(cached_done, sizeof(cached_done), TIME_UNKNOWN);
    PrintTime(cached_start, sizeof(cached_start), TIME_UNKNOWN);
    PrintTime(cached_duration, sizeof(cached_duration), TIME_UNKNOWN);
}

bool ObjectiveTimerWindow::Objective::IsStarted() const { 
    return start != TIME_UNKNOWN;
}
bool ObjectiveTimerWindow::Objective::IsDone() const { 
    return done != TIME_UNKNOWN;
}
void ObjectiveTimerWindow::Objective::SetStarted() {
    if (start == TIME_UNKNOWN) {
        if (GW::Map::GetInstanceType() != GW::Constants::InstanceType::Explorable) {
            start = 0; // still loading, just set to 0
        }
        else {
            start = GW::Map::GetInstanceTime();
        }
    }
    PrintTime(cached_start, sizeof(cached_start), start);
    status = Started;
}
void ObjectiveTimerWindow::Objective::SetDone() {
    if (start == TIME_UNKNOWN) SetStarted(); // something went wrong
    if (done == TIME_UNKNOWN)
        done = GW::Map::GetInstanceTime();
    PrintTime(cached_done, sizeof(cached_done), done);
    duration = done - start;
    PrintTime(cached_duration, sizeof(cached_duration), duration);
    status = Completed;
}

void ObjectiveTimerWindow::Objective::Update() {
    if (start == TIME_UNKNOWN) {
        PrintTime(cached_duration, sizeof(cached_duration), TIME_UNKNOWN);
    } else if (done == TIME_UNKNOWN) {
        PrintTime(cached_duration, sizeof(cached_duration), GW::Map::GetInstanceTime() - start);
    }
}
void ObjectiveTimerWindow::Objective::Draw() {

    switch (status) {
    case ObjectiveTimerWindow::Objective::NotStarted:
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        break;
    case ObjectiveTimerWindow::Objective::Started:
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        break;
    case ObjectiveTimerWindow::Objective::Completed:
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.0f, 1.0f));
        break;
    case ObjectiveTimerWindow::Objective::Failed:
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
        break;
    default:
        break;
    }
    auto& style = ImGui::GetStyle();
    style.ButtonTextAlign.x = 0.0f;
    if (ImGui::Button(name, ImVec2(GetLabelWidth(), 0))) {
        char buf[256];
        snprintf(buf, 256, "[%s] ~ Start: %s ~ End: %s ~ Time: %s",
            name, cached_start, cached_done, cached_duration);
        GW::Chat::SendChat('#', buf);
    }
    style.ButtonTextAlign.x = 0.5f;
    ImGui::PopStyleColor();
    float ts_width = GetTimestampWidth();
    float offset = 0.0f;
    
    ImGui::PushItemWidth(ts_width);
    if (show_start_column) {
        ImGui::SameLine();
        ImGui::InputText("##start", cached_start, sizeof(cached_start), ImGuiInputTextFlags_ReadOnly);
        // ImGui::SameLine(offset += ts_width + style.ItemInnerSpacing.x, -1);
    }
    if (show_end_column) {
        ImGui::SameLine();
        ImGui::InputText("##end", cached_done, sizeof(cached_done), ImGuiInputTextFlags_ReadOnly);
        //ImGui::SameLine();//ImGui::SameLine(offset += ts_width + style.ItemInnerSpacing.x, -1);
    }
    if (show_time_column) {
        ImGui::SameLine();
        ImGui::InputText("##time", cached_duration, sizeof(cached_duration), ImGuiInputTextFlags_ReadOnly);
        //ImGui::SameLine();//ImGui::SameLine(offset += ts_width + style.ItemInnerSpacing.x, -1);
    }
}

void ObjectiveTimerWindow::ObjectiveSet::Update() {
    if (!active) return;

    if (GW::Map::GetInstanceType() == GW::Constants::InstanceType::Explorable) {
        time = GW::Map::GetInstanceTime();
        ::PrintTime(cached_time, sizeof(cached_time), time, false);
    }

    for (Objective& obj : objectives) {
        obj.Update();
    }
}
void ObjectiveTimerWindow::ObjectiveSet::CheckSetDone() {
    bool done = true;
    for (const Objective& obj : objectives) {
        if (obj.done == TIME_UNKNOWN) {
            done = false;
            break;
        }
    }
    if (done) {
        active = false;
    }
}

ObjectiveTimerWindow::ObjectiveSet::ObjectiveSet() : ui_id(cur_ui_id++) {
	name[0] = 0;
	GetLocalTime(&system_time);
}

bool ObjectiveTimerWindow::ObjectiveSet::Draw() {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s - %s###header%d", name, cached_time ? cached_time : "--:--", ui_id);

    bool is_open = true;
    const auto& style = ImGui::GetStyle();
    float offset = 0;
    float ts_width = GetTimestampWidth();
    if (ImGui::CollapsingHeader(buf, &is_open, ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID(ui_id);
        offset = style.WindowPadding.x + GetLabelWidth() + style.ItemInnerSpacing.x + ImGui::GetCurrentWindow()->DC.GroupOffset.x - GetTimestampWidth() - style.ItemInnerSpacing.x;
        if (show_start_column) {
            ImGui::SameLine(offset += GetTimestampWidth() + style.ItemInnerSpacing.x);
            ImGui::Text("Start");
        }
        if (show_end_column) {
            ImGui::SameLine(offset += GetTimestampWidth() + style.ItemInnerSpacing.x);
            ImGui::Text("End");
        }
        if (show_time_column) {
            ImGui::SameLine(offset += GetTimestampWidth() + style.ItemInnerSpacing.x);
            ImGui::Text("Time");
        }
        for (Objective& objective : objectives) {
            objective.Draw();
        }
		ImGui::PopID();
	}
    return is_open;
}
