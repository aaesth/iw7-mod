#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"
#include "game/dvars.hpp"

#include "fastfiles.hpp"
#include "filesystem.hpp"
#include "dvars.hpp"

#include <utils/hook.hpp>
#include <utils/string.hpp>

namespace patches
{
	namespace
	{
		utils::hook::detour com_register_common_dvars_hook;
		utils::hook::detour com_game_mode_supports_feature_hook;
		utils::hook::detour cg_set_client_dvar_from_server_hook;
		utils::hook::detour live_get_map_index_hook;
		utils::hook::detour content_do_we_have_content_pack_hook;
		utils::hook::detour init_network_dvars_hook;

		std::string get_login_username()
		{
			char username[UNLEN + 1];
			DWORD username_len = UNLEN + 1;
			if (!GetUserNameA(username, &username_len))
			{
				return "Unknown Soldier";
			}

			return std::string{ username, username_len - 1 };
		}

		void com_register_common_dvars_stub()
		{
			game::dvar_t* name_dvar;
			game::dvar_t* com_maxfps;

			name_dvar = game::Dvar_RegisterString("name", get_login_username().data(), game::DVAR_FLAG_SAVED, "Player name.");

			if (game::environment::is_dedi())
			{
				com_maxfps = game::Dvar_RegisterInt("com_maxfps", 85, 0, 100, game::DVAR_FLAG_NONE, "Cap frames per second");
			}
			else
			{
				com_maxfps = game::Dvar_RegisterInt("com_maxfps", 0, 0, 1000, game::DVAR_FLAG_SAVED, "Cap frames per second");
			}

			*reinterpret_cast<game::dvar_t**>(0x146005758) = com_maxfps;
			dvars::disable::re_register("com_maxfps");
			dvars::disable::de_register("com_maxfps");

			return com_register_common_dvars_hook.invoke<void>();
		}

		bool com_game_mode_supports_feature_stub(game::Com_GameMode_Feature feature)
		{
			if (feature == game::FEATURE_GRAVITY)
			{
				return true;
			}
			else if (feature == game::FEATURE_TIMESCALE)
			{
				return true;
			}

			return com_game_mode_supports_feature_hook.invoke<bool>(feature);
		}

		const char* live_get_local_client_name()
		{
			return game::Dvar_FindVar("name")->current.string;
		}

		std::vector<std::string> dvar_save_variables;
		void dvar_write_single_variable(const game::dvar_t* dvar, int* user_data)
		{
			if ((dvar->flags & game::DVAR_FLAG_SAVED) != 0)
			{
				const char* val = game::Dvar_DisplayableLatchedValue(dvar);
				auto h = *user_data;

				std::string dvar_name = dvars::dvar_get_name(dvar);
				if (dvar_name.empty())
				{
					game::FS_Printf(h, "setcl %d \"%s\"\n", dvar->checksum, val);
				}
				else
				{
					dvar_save_variables.push_back(dvar_name);
				}
			}
		}

		void dvar_write_variables_stub(int handle)
		{
			dvar_save_variables.clear();

			int* user_data = &handle;
			game::Dvar_ForEach(dvar_write_single_variable, user_data);

			std::sort(dvar_save_variables.begin(), dvar_save_variables.end()); // alphabetize sort
			for (size_t i = 0; i < dvar_save_variables.size(); i++)
			{
				const auto* dvar_name = dvar_save_variables.at(i).data();
				const auto* dvar = game::Dvar_FindVar(dvar_name);
				const char* val = game::Dvar_DisplayableLatchedValue(dvar);
				game::FS_Printf(handle, "seta %s \"%s\"\n", dvar_name, val);
			}
		}

		void missing_content_error_stub(int, const char*)
		{
			game::Com_Error(game::ERR_DROP, utils::string::va("MISSING FILE\n%s.ff",
				fastfiles::get_current_fastfile().data()));
		}

		const char* stored_mapname;
		int live_get_map_index_stub(const char* map)
		{
			stored_mapname = map;
			return live_get_map_index_hook.invoke<int>(map);
		}

		bool content_do_we_have_content_pack_stub(int index)
		{
			if (stored_mapname != nullptr && !fastfiles::exists(stored_mapname))
			{
				stored_mapname = nullptr;
				return false;
			}
			return content_do_we_have_content_pack_hook.invoke<bool>(index);
		}

		void cg_set_client_dvar_from_server_stub(void* client_num, void* cgame_glob, const char* dvar_checksum, const char* value)
		{
			unsigned int checksum = static_cast<unsigned int>(atoi(dvar_checksum));
			auto* dvar = game::Dvar_FindMalleableVar(checksum);

			static unsigned int cg_fov_checksum = game::Dvar_GenerateChecksum("cg_fov");
			static unsigned int cg_fovScale_checksum = game::Dvar_GenerateChecksum("cg_fovScale");

			if (checksum == cg_fov_checksum ||
				checksum == cg_fovScale_checksum)
			{
				return;
			}

			// register new dvar
			if (!dvar)
			{
				game::Dvar_SetFromStringByChecksum(checksum, value, game::DvarSetSource::DVAR_SOURCE_EXTERNAL);
			}
			// only set if dvar has no flags or has external flag
			else if (dvar->flags == game::DVAR_FLAG_NONE ||
				(dvar->flags & game::DVAR_FLAG_EXTERNAL) != 0)
			{
				game::Dvar_SetFromStringFromSource(dvar, value, game::DvarSetSource::DVAR_SOURCE_EXTERNAL);
			}

			// original code
			unsigned int index = 0;
			auto result = utils::hook::invoke<__int64>(0x140B7AC60, dvar, &index); // NetConstStrings_SV_GetNetworkDvarIndex
			if (result)
			{
				std::string index_str = std::to_string(index);
				return cg_set_client_dvar_from_server_hook.invoke<void>(client_num, cgame_glob, index_str.data(), value);
			}
		}

		game::dvar_t* get_client_dvar(const char* name)
		{
			game::dvar_t* dvar = game::Dvar_FindVar(name);
			if (!dvar)
			{
				static game::dvar_t dummy{ 0 };
				dummy.checksum = game::Dvar_GenerateChecksum(name);
				return &dummy;
			}
			return dvar;
		}

		bool get_client_dvar_checksum(game::dvar_t* dvar, unsigned int* checksum)
		{
			*checksum = dvar->checksum;
			return true;
		}

		char* db_read_raw_file_stub(const char* filename, char* buf, const int size)
		{
			std::string file_name = filename;
			if (file_name.find(".cfg") == std::string::npos)
			{
				file_name.append(".cfg");
			}

			std::string buffer{};
			if (filesystem::read_file(file_name, &buffer))
			{
				snprintf(buf, size, "%s\n", buffer.data());
				return buf;
			}

			return game::DB_ReadRawFile(filename, buf, size);
		}

		void cbuf_execute_buffer_internal_stub(int local_client_num, int controller_index, char* buffer, [[maybe_unused]]void* callback)
		{
			game::Dvar_OverrideCheatProtection(0);
			game::Cbuf_ExecuteBufferInternal(local_client_num, controller_index, buffer, game::Cmd_ExecuteSingleCommand);
			game::Dvar_OverrideCheatProtection(1);
		}

		void init_network_dvars_stub(game::dvar_t* dvar)
		{
			//init_network_dvars_hook.invoke<void>(dvar);
		}

		void disconnect()
		{
			utils::hook::invoke<void>(0x140C58E20); // SV_MainMP_MatchEnd
		}
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			// register custom dvars
			com_register_common_dvars_hook.create(0x140BADF30, com_register_common_dvars_stub);

			// patch some features
			com_game_mode_supports_feature_hook.create(game::Com_GameMode_SupportsFeature, com_game_mode_supports_feature_stub);

			// get client name from dvar
			utils::hook::jump(0x140D32770, live_get_local_client_name);

			// write better config
			utils::hook::jump(0x140BB2A90, dvar_write_variables_stub);

			// show missing fastfiles
			utils::hook::call(0x1403BBD4B, missing_content_error_stub);

			// show missing map
			stored_mapname = nullptr;
			live_get_map_index_hook.create(0x140CE72C0, live_get_map_index_stub);
			content_do_we_have_content_pack_hook.create(0x140CE8550, content_do_we_have_content_pack_stub);

			// make setclientdvar behave like older games
			cg_set_client_dvar_from_server_hook.create(0x140856D70, cg_set_client_dvar_from_server_stub);
			utils::hook::call(0x140B0A9BB, get_client_dvar_checksum); // setclientdvar
			utils::hook::call(0x140B0ACD7, get_client_dvar_checksum); // setclientdvars
			utils::hook::call(0x140B0A984, get_client_dvar); // setclientdvar
			utils::hook::call(0x140B0AC9F, get_client_dvar); // setclientdvars
			utils::hook::set<uint8_t>(0x140B0A9AC, 0xEB); // setclientdvar
			utils::hook::set<uint8_t>(0x140B0ACC8, 0xEB); // setclientdvars

			// Allow executing custom cfg files with the "exec" command
			utils::hook::call(0x140B7CEF9, db_read_raw_file_stub);
			// Add cheat override to exec
			utils::hook::call(0x140B7CF11, cbuf_execute_buffer_internal_stub);

			// don't register every replicated dvar as a network dvar
			init_network_dvars_hook.create(0x140B7A920, init_network_dvars_stub);

			// some [data validation] anti tamper thing that kills performance
			dvars::override::register_int("dvl", 0, 0, 0, game::DVAR_FLAG_READ);

			// killswitches
			dvars::override::register_bool("mission_team_contracts_enabled", true, game::DVAR_FLAG_READ);
			dvars::override::register_bool("killswitch_store", false, game::DVAR_FLAG_READ);
			dvars::override::register_bool("killswitch_quartermaster", false, game::DVAR_FLAG_READ);
			dvars::override::register_bool("killswitch_cod_points", false, game::DVAR_FLAG_READ);
			dvars::override::register_bool("killswitch_custom_emblems", false, game::DVAR_FLAG_READ);
			dvars::override::register_bool("killswitch_matchID", true, game::DVAR_FLAG_READ);
			dvars::override::register_bool("killswitch_mp_leaderboards", true, game::DVAR_FLAG_READ);
			dvars::override::register_bool("killswitch_cp_leaderboards", true, game::DVAR_FLAG_READ);
			dvars::override::register_bool("killswitch_streak_variants", false, game::DVAR_FLAG_READ);
			dvars::override::register_bool("killswitch_blood_anvil", false, game::DVAR_FLAG_READ);

			// announcer packs
			if (!game::environment::is_dedi())
			{
				dvars::override::register_bool("killswitch_announcers", false, game::DVAR_FLAG_READ);
				dvars::override::register_int("igs_announcer", 0x1F, 0, 0x7FFFFFFF, game::DVAR_FLAG_READ); // show all announcer packs
			}

			// disable cod account
			dvars::override::register_bool("enable_cod_account", false, game::DVAR_FLAG_READ);

			// enable boss battles
			dvars::override::register_bool("online_zombie_boss_battle", true, game::DVAR_FLAG_READ);
			dvars::override::register_bool("online_zombie_boss_zmb", true, game::DVAR_FLAG_READ);
			dvars::override::register_bool("online_zombie_boss_rave", true, game::DVAR_FLAG_READ);
			dvars::override::register_bool("online_zombie_boss_disco", true, game::DVAR_FLAG_READ);
			dvars::override::register_bool("online_zombie_boss_town", true, game::DVAR_FLAG_READ);
			dvars::override::register_bool("online_zombie_boss_final", true, game::DVAR_FLAG_READ);
			dvars::override::register_bool("online_zombie_boss_dc", true, game::DVAR_FLAG_READ);

			// uncheat protect gamepad-related dvars
			dvars::override::register_float("gpad_button_deadzone", 0.13f, 0, 1, game::DVAR_FLAG_SAVED);
			dvars::override::register_float("gpad_stick_deadzone_min", 0.2f, 0, 1, game::DVAR_FLAG_SAVED);
			dvars::override::register_float("gpad_stick_deadzone_max", 0.01f, 0, 1, game::DVAR_FLAG_SAVED);
			dvars::override::register_float("gpad_stick_pressed", 0.4f, 0, 1, game::DVAR_FLAG_SAVED);
			dvars::override::register_float("gpad_stick_pressed_hysteresis", 0.1f, 0, 1, game::DVAR_FLAG_SAVED);

			// disable host migration
			utils::hook::jump(0x140C5A200, disconnect);

			// precache is always allowed
			utils::hook::set(0x1406D5280, 0xC301B0); // NetConstStrings_IsPrecacheAllowed

			utils::hook::nop(0x140E6A2FB, 2); // don't wait for occlusion query to succeed (forever loop)
			utils::hook::nop(0x140E6A30C, 2); // ^
		}
	};
}

REGISTER_COMPONENT(patches::component)