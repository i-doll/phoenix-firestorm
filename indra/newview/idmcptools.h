/**
 * @file idmcptools.h
 * @brief <ID> MCP server: registration entry points for each tool-area file.
 *
 * Part of Five's custom Firestorm fork. Custom code carries an `ID` prefix.
 */

#ifndef ID_IDMCPTOOLS_H
#define ID_IDMCPTOOLS_H

#include "idmcptoolregistry.h"

void idmcp_register_rlv_tools(IDMCPToolRegistry& reg);
void idmcp_register_inventory_tools(IDMCPToolRegistry& reg);
void idmcp_register_appearance_tools(IDMCPToolRegistry& reg);
void idmcp_register_gesture_tools(IDMCPToolRegistry& reg);
void idmcp_register_wearable_tools(IDMCPToolRegistry& reg);
void idmcp_register_profile_tools(IDMCPToolRegistry& reg);
void idmcp_register_search_tools(IDMCPToolRegistry& reg);
void idmcp_register_avatars_tools(IDMCPToolRegistry& reg);
void idmcp_register_manage_tools(IDMCPToolRegistry& reg);
void idmcp_register_group_tools(IDMCPToolRegistry& reg);
void idmcp_register_upload_tools(IDMCPToolRegistry& reg);
void idmcp_register_chat_tools(IDMCPToolRegistry& reg);
void idmcp_register_movement_tools(IDMCPToolRegistry& reg);
void idmcp_register_vision_tools(IDMCPToolRegistry& reg);
void idmcp_register_im_tools(IDMCPToolRegistry& reg);
void idmcp_register_notifications_tools(IDMCPToolRegistry& reg);
void idmcp_register_money_tools(IDMCPToolRegistry& reg);

#endif // ID_IDMCPTOOLS_H
