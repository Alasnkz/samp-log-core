#include "CAmxDebugManager.hpp"
#include "SampConfigReader.hpp"
#include "LogConfigReader.hpp"

#include <cassert>
#include <tinydir.h>
#include <algorithm>
#include <vector>

using samplog::AmxFuncCallInfo;


CAmxDebugManager::CAmxDebugManager()
{
	string use_debuginfo;
	if (LogConfigReader::Get()->GetGlobalConfig().DisableDebugInfo)
	{
		// disable whole debug info functionality
		m_DisableDebugInfo = true;
		return;
	}

	std::vector<string> gamemodes;
	if (!SampConfigReader::Get()->GetGamemodeList(gamemodes))
		return;

	for (auto &g : gamemodes)
	{
		string amx_filepath = "gamemodes/" + g + ".amx";
		InitDebugData(amx_filepath);
	}
	

	//load ALL filterscripts (there's no other way since filterscripts can be dynamically (un)loaded
	InitDebugDataDir("filterscripts");
}

CAmxDebugManager::~CAmxDebugManager()
{
	for (auto &a : m_AvailableDebugInfo)
	{
		delete a.first;
		delete a.second;
	}
}

void CAmxDebugManager::InitDebugDataDir(string directory)
{
	tinydir_dir dir;
	tinydir_open(&dir, directory.c_str());

	while (dir.has_next)
	{
		tinydir_file file;
		tinydir_readfile(&dir, &file);
		
		if (file.is_dir && file.name[0] != '.')
			InitDebugDataDir(file.path);
		else if (!strcmp(file.extension, "amx"))
			InitDebugData(file.path);

		tinydir_next(&dir);
	}

	tinydir_close(&dir);
}

bool CAmxDebugManager::InitDebugData(string filepath)
{
	FILE* amx_file = fopen(filepath.c_str(), "rb");
	if (amx_file == nullptr)
		return false;

	/*
	  The following two lines are stripped from AMX helper function "aux_LoadProgram".
	  There are some additional endianess checks and alignments, but these are only
	  important if the system is using big endian. We assume that this library always runs on
	  litte-endian machines, since the SA-MP server only runs on x86(-64) architecture.
	*/
	AMX_HEADER hdr;
	fread(&hdr, sizeof hdr, 1, amx_file);

	/*if (hdr.magic != AMX_MAGIC) {
		fclose(fp);
		return AMX_ERR_FORMAT;
	}*/

	AMX_DBG amxdbg;
	//dbg_LoadInfo already seeks to the beginning of the file
	int error = dbg_LoadInfo(&amxdbg, amx_file);

	fclose(amx_file);

	if (error == AMX_ERR_NONE)
		m_AvailableDebugInfo.emplace(new AMX_HEADER(hdr), new AMX_DBG(amxdbg));

	return (error == AMX_ERR_NONE);
}

void CAmxDebugManager::RegisterAmx(AMX *amx)
{
	if (m_DisableDebugInfo)
		return;

	if (m_AmxDebugMap.find(amx) != m_AmxDebugMap.end()) //amx already registered
		return;

	for (auto &d : m_AvailableDebugInfo)
	{
		if (memcmp(d.first, amx->base, sizeof(AMX_HEADER)) == 0)
		{
			m_AmxDebugMap.emplace(amx, d.second);
			break;
		}
	}
}

void CAmxDebugManager::EraseAmx(AMX *amx)
{
	if (m_DisableDebugInfo)
		return;

	m_AmxDebugMap.erase(amx);
}

bool CAmxDebugManager::GetFunctionCall(AMX * const amx, ucell address, AmxFuncCallInfo &dest)
{
	if (m_DisableDebugInfo)
		return false;

	auto it = m_AmxDebugMap.find(amx);
	if (it == m_AmxDebugMap.end())
		return false;

	AMX_DBG *amx_dbg = it->second;

	{
		// workaround for possible overflow of amx_dbg->hdr->lines
		// taken from Zeex' crashdetect plugin code
		int num_lines = (
			reinterpret_cast<unsigned char*>(amx_dbg->symboltbl[0]) -
			reinterpret_cast<unsigned char*>(amx_dbg->linetbl)
			) / sizeof(AMX_DBG_LINE);
		int index = 0;
		while (index < num_lines && amx_dbg->linetbl[index].address <= address)
			index++;

		if (index >= num_lines)
			return false; // invalid address

		if (--index < 0)
			return false; // not found

		dest.line = amx_dbg->linetbl[index].line + 1;
	}

	if (dbg_LookupFile(amx_dbg, address, &(dest.file)) != AMX_ERR_NONE)
		return false;

	if (dbg_LookupFunction(amx_dbg, address, &(dest.function)) != AMX_ERR_NONE)
		return false;

	return true;
}

bool CAmxDebugManager::GetFunctionCallTrace(AMX * const amx, std::vector<AmxFuncCallInfo> &dest)
{
	if (m_DisableDebugInfo)
		return false;

	auto it = m_AmxDebugMap.find(amx);
	if (it == m_AmxDebugMap.end())
		return false;

	AMX_DBG *amx_dbg = it->second;
	AmxFuncCallInfo call_info;

	if (!GetFunctionCall(amx, amx->cip, call_info))
		return false;

	dest.push_back(call_info);

	AMX_HEADER *base = reinterpret_cast<AMX_HEADER *>(amx->base);
	cell dat = reinterpret_cast<cell>(amx->base + base->dat);
	cell cod = reinterpret_cast<cell>(amx->base + base->cod);

	cell frm_addr = amx->frm;

	while (true)
	{
		cell ret_addr = *(reinterpret_cast<cell *>(dat + frm_addr + sizeof(cell)));

		if (ret_addr == 0)
			break;

		if (GetFunctionCall(amx, ret_addr, call_info))
			dest.push_back(call_info);
		else
			dest.push_back({ 0, "<unknown>", "<unknown>" });

		frm_addr = *(reinterpret_cast<cell *>(dat + frm_addr));
		if (frm_addr == 0)
			break;
	}

	//HACK: for some reason the oldest/highest call (not cip though) 
	//      has a slightly incorrect ret_addr
	if (dest.size() > 1)
		dest.back().line--;

	return true;
}


void samplog_RegisterAmx(AMX *amx)
{
	CAmxDebugManager::Get()->RegisterAmx(amx);
}

void samplog_EraseAmx(AMX *amx)
{
	CAmxDebugManager::Get()->EraseAmx(amx);
}

bool samplog_GetLastAmxFunctionCall(AMX * const amx, samplog::AmxFuncCallInfo *destination)
{
	if (destination == nullptr)
		return false;

	return CAmxDebugManager::Get()->GetFunctionCall(amx, amx->cip, *destination);
}

unsigned int samplog_GetAmxFunctionCallTrace(AMX * const amx, samplog::AmxFuncCallInfo * destination, unsigned int max_size)
{
	if (destination == nullptr || max_size == 0)
		return 0;

	std::vector<AmxFuncCallInfo> calls;
	if (!CAmxDebugManager::Get()->GetFunctionCallTrace(amx, calls))
		return 0;
	
	size_t size = std::min(calls.size(), max_size);
	for (size_t i = 0; i < size; ++i)
		destination[i] = calls.at(i);

	return size;
}
