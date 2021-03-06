/*
	Copyright 2010 OpenRTMFP

	This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License received along this program for more
	details (or else see http://www.gnu.org/licenses/).

	This file is a part of Cumulus.
*/

#include "Server.h"
#include "LUAClient.h"
#include "LUAPublication.h"
#include "LUAPublications.h"
#include "LUAListener.h"
#include "LUAFlowWriter.h"
#include "LUAInvoker.h"
#include "LUAClients.h"
#include "LUAGroups.h"
#include "LUAGroup.h"
#include "LUAEdges.h"
#include "LUAAMFObjectWriter.h"

using namespace std;
using namespace Poco;
using namespace Cumulus;

const string Server::WWWPath;

Server::Server(ApplicationKiller& applicationKiller,const Util::AbstractConfiguration& configurations) : RTMFPServer(configurations.getInt("cores",0)),_pState(Script::CreateState()),_blacklist(configurations.getString("application.dir","./")+"blacklist",*this),_applicationKiller(applicationKiller),_pService(NULL),
	mails(*this,configurations.getString("smtp.host","localhost"),configurations.getInt("smtp.port",SMTPSession::SMTP_PORT),configurations.getInt("smtp.timeout",60)) {
	
	File((string&)WWWPath = configurations.getString("application.dir","./")+"www").createDirectory();
	Service::InitGlobalTable(_pState);
	SCRIPT_BEGIN(_pState)
		SCRIPT_CREATE_PERSISTENT_OBJECT(Invoker,LUAInvoker,*this)
		readNextConfig(_pState,configurations,"");
		lua_setglobal(_pState,"cumulus.configs");
	SCRIPT_END
}


bool Server::readNextConfig(lua_State* pState,const Util::AbstractConfiguration& configurations,const string& root) {
	Util::AbstractConfiguration::Keys keys;
	configurations.keys(root,keys);
	if(keys.size()==0)
		return false;
	Util::AbstractConfiguration::Keys::const_iterator it;
	lua_newtable(pState);
	for(it=keys.begin();it!=keys.end();++it) {
		string key = root.empty() ? (*it) : (root+"."+*it);
		if(!readNextConfig(pState,configurations,key)) {
			try {
				string value = configurations.getString(key);
				if(value=="false")
					lua_pushboolean(_pState,0);
				else
					lua_pushstring(_pState,value.c_str());
			} catch(Exception& ex) {
				DEBUG("Configuration scripting conversion: %s",ex.displayText().c_str());
				lua_pushnil(_pState);
			}
		}
		lua_setfield(_pState,-2,(*it).c_str());
	}
	return true;
}


Server::~Server() {
	mails.clear();
	Script::CloseState(_pState);
}

void Server::onStart() {
	_pService = new Service(_pState,"");
}
void Server::onStop() {
	// delete service
	if(_pService) {
		delete _pService;
		_pService=NULL;
	}
	_applicationKiller.kill();
}


void Server::manage() {
	RTMFPServer::manage();
	_pService->refresh();
	SCRIPT_BEGIN(_pService->open())
		SCRIPT_FUNCTION_BEGIN("onManage")
			SCRIPT_FUNCTION_CALL
		SCRIPT_FUNCTION_END
	SCRIPT_END
}


//// CLIENT_HANDLER /////
bool Server::onConnection(Client& client,AMFReader& parameters,AMFObjectWriter& response) {
	// Here you can read custom client http parameters in reading "client.parameters".
	Service* pService = _pService->get(client.path);
	if(!pService) {
		client.writer().writeErrorResponse("Connect.InvalidApp","Applicaton " + client.path + " doesn't exist");
		return false;
	}

	SCRIPT_BEGIN(pService->open())
		SCRIPT_FUNCTION_BEGIN("onConnection")
			SCRIPT_WRITE_PERSISTENT_OBJECT(Client,LUAClient,client)
			SCRIPT_WRITE_OBJECT(AMFObjectWriter,LUAAMFObjectWriter,response)
			SCRIPT_WRITE_AMF(parameters,0)
			SCRIPT_FUNCTION_CALL
			if(SCRIPT_CAN_READ && (SCRIPT_NEXT_TYPE==LUA_TSTRING || !SCRIPT_READ_BOOL(true))) {
				string rejected("client rejected");
				if(SCRIPT_CAN_READ)
					rejected = SCRIPT_READ_STRING("client rejected");
				pService=NULL;
				client.writer().writeErrorResponse("Connect.Rejected",rejected);
			}
		SCRIPT_FUNCTION_END
		if(SCRIPT_LAST_ERROR) {
			client.writer().writeErrorResponse("Connect.Error",SCRIPT_LAST_ERROR);
			pService=NULL;
		}
		if(!pService)
			LUAClient::Clear(_pState,client);
	SCRIPT_END

	if(pService) {
		if(!pService->lastError.empty()) {
			client.writer().writeErrorResponse("Connect.Error",pService->lastError);
			return false;
		}
		client.pinObject<Service>(*pService);
		++pService->count;
		return true;
	}
	return false;
}

void Server::onFailed(const Client& client,const string& error) {
	WARN("Client failed : %s",error.c_str());
	SCRIPT_BEGIN(client.object<Service>()->open())
		SCRIPT_FUNCTION_BEGIN("onFailed")
			SCRIPT_WRITE_PERSISTENT_OBJECT(Client,LUAClient,client)
			SCRIPT_WRITE_STRING(error.c_str())
			SCRIPT_FUNCTION_CALL
		SCRIPT_FUNCTION_END
	SCRIPT_END
}

void Server::onDisconnection(const Client& client) {
	Service& service = *client.object<Service>();
	SCRIPT_BEGIN(service.open())
		SCRIPT_FUNCTION_BEGIN("onDisconnection")
			SCRIPT_WRITE_PERSISTENT_OBJECT(Client,LUAClient,client)
			SCRIPT_FUNCTION_CALL
		SCRIPT_FUNCTION_END
	SCRIPT_END
	LUAClient::Clear(_pState,client);
	--service.count;
}

bool Server::onMessage(Client& client,const string& name,AMFReader& reader) {
	DEBUG("onMessage %s",name.c_str());

	bool result = false;

	SCRIPT_BEGIN(client.object<Service>()->open())
		SCRIPT_MEMBER_FUNCTION_BEGIN(Client,LUAClient,client,name.c_str())
			SCRIPT_WRITE_AMF(reader,0)
			result = true;
			SCRIPT_FUNCTION_CALL
			if(SCRIPT_CAN_READ) {
				Script::ReadAMF(_pState,client.writer().writeAMFResult(),1);
				++__args;
			}
		SCRIPT_FUNCTION_END
		if(SCRIPT_LAST_ERROR)
			client.writer().writeErrorResponse("Call.Failed",SCRIPT_LAST_ERROR);	
	SCRIPT_END

	return result;
}

//// PUBLICATION_HANDLER /////
bool Server::onPublish(Client& client,const Publication& publication,string& error) {
	if(client == this->id)
		return true;

	bool result=true;
	SCRIPT_BEGIN(client.object<Service>()->open())
		SCRIPT_FUNCTION_BEGIN("onPublish")
			SCRIPT_WRITE_PERSISTENT_OBJECT(Client,LUAClient,client)
			SCRIPT_WRITE_PERSISTENT_OBJECT(Publication,LUAPublication,publication)
			SCRIPT_FUNCTION_CALL
			if(SCRIPT_CAN_READ)
				result = SCRIPT_READ_BOOL(true);
		SCRIPT_FUNCTION_END
		if(SCRIPT_LAST_ERROR) {
			error = SCRIPT_LAST_ERROR;
			result = false;
		}
		if(!result)
			LUAPublication::Clear(_pState,publication);
	SCRIPT_END
	return result;
}

void Server::onUnpublish(Client& client,const Publication& publication) {
	if(client != this->id) {
		SCRIPT_BEGIN(client.object<Service>()->open())
			SCRIPT_FUNCTION_BEGIN("onUnpublish")
				SCRIPT_WRITE_PERSISTENT_OBJECT(Client,LUAClient,client)
				SCRIPT_WRITE_PERSISTENT_OBJECT(Publication,LUAPublication,publication)
				SCRIPT_FUNCTION_CALL
			SCRIPT_FUNCTION_END
		SCRIPT_END
	}
	if(publication.listeners.count()==0)
		LUAPublication::Clear(_pState,publication);
}

bool Server::onSubscribe(Client& client,const Listener& listener,string& error) { 
	bool result=true;
	SCRIPT_BEGIN(client.object<Service>()->open())
		SCRIPT_FUNCTION_BEGIN("onSubscribe")
			SCRIPT_WRITE_PERSISTENT_OBJECT(Client,LUAClient,client)
			SCRIPT_WRITE_PERSISTENT_OBJECT(Listener,LUAListener,listener)
			SCRIPT_FUNCTION_CALL
			if(SCRIPT_CAN_READ)
				result = SCRIPT_READ_BOOL(true);
		SCRIPT_FUNCTION_END
		if(SCRIPT_LAST_ERROR) {
			error = SCRIPT_LAST_ERROR;
			result = false;
		}
		if(!result)
			LUAListener::Clear(_pState,listener);
	SCRIPT_END
	return result;
}

void Server::onUnsubscribe(Client& client,const Listener& listener) {
	SCRIPT_BEGIN(client.object<Service>()->open())
		SCRIPT_FUNCTION_BEGIN("onUnsubscribe")
			SCRIPT_WRITE_PERSISTENT_OBJECT(Client,LUAClient,client)
			SCRIPT_WRITE_PERSISTENT_OBJECT(Listener,LUAListener,listener)
			SCRIPT_FUNCTION_CALL
		SCRIPT_FUNCTION_END
	SCRIPT_END
	if(listener.publication.publisherId()==0 && listener.publication.listeners.count()==0)
		LUAPublication::Clear(_pState,listener.publication);
	LUAListener::Clear(_pState,listener);
}

void Server::onAudioPacket(Client& client,const Publication& publication,UInt32 time,PacketReader& packet) {
	if(client == this->id)
		return;
	SCRIPT_BEGIN(client.object<Service>()->open())
		SCRIPT_FUNCTION_BEGIN("onAudioPacket")
			SCRIPT_WRITE_PERSISTENT_OBJECT(Client,LUAClient,client)
			SCRIPT_WRITE_PERSISTENT_OBJECT(Publication,LUAPublication,publication)
			SCRIPT_WRITE_NUMBER(time)
			SCRIPT_WRITE_BINARY(packet.current(),packet.available())
			SCRIPT_FUNCTION_CALL
		SCRIPT_FUNCTION_END
	SCRIPT_END
}

void Server::onVideoPacket(Client& client,const Publication& publication,UInt32 time,PacketReader& packet) {
	if(client == this->id)
		return;
	SCRIPT_BEGIN(client.object<Service>()->open())
		SCRIPT_FUNCTION_BEGIN("onVideoPacket")
			SCRIPT_WRITE_PERSISTENT_OBJECT(Client,LUAClient,client)
			SCRIPT_WRITE_PERSISTENT_OBJECT(Publication,LUAPublication,publication)
			SCRIPT_WRITE_NUMBER(time)
			SCRIPT_WRITE_BINARY(packet.current(),packet.available())
			SCRIPT_FUNCTION_CALL
		SCRIPT_FUNCTION_END
	SCRIPT_END
}

void Server::onDataPacket(Client& client,const Publication& publication,const string& name,PacketReader& packet) {
	if(client == this->id)
		return;
	SCRIPT_BEGIN(client.object<Service>()->open())
		SCRIPT_FUNCTION_BEGIN("onDataPacket")
			SCRIPT_WRITE_PERSISTENT_OBJECT(Client,LUAClient,client)
			SCRIPT_WRITE_PERSISTENT_OBJECT(Publication,LUAPublication,publication)
			SCRIPT_WRITE_STRING(name.c_str())
			SCRIPT_WRITE_BINARY(packet.current(),packet.available())
			SCRIPT_FUNCTION_CALL
		SCRIPT_FUNCTION_END
	SCRIPT_END
}

void Server::onJoinGroup(Client& client,Group& group) {
	SCRIPT_BEGIN(client.object<Service>()->open())
		SCRIPT_FUNCTION_BEGIN("onJoinGroup")
			SCRIPT_WRITE_PERSISTENT_OBJECT(Client,LUAClient,client)
			SCRIPT_WRITE_PERSISTENT_OBJECT(Group,LUAGroup,group)
			SCRIPT_FUNCTION_CALL
		SCRIPT_FUNCTION_END
	SCRIPT_END
}

void Server::onUnjoinGroup(Client& client,Group& group) {
	SCRIPT_BEGIN(client.object<Service>()->open())
		SCRIPT_FUNCTION_BEGIN("onUnjoinGroup")
			SCRIPT_WRITE_PERSISTENT_OBJECT(Client,LUAClient,client)
			SCRIPT_WRITE_PERSISTENT_OBJECT(Group,LUAGroup,group)
			SCRIPT_FUNCTION_CALL
		SCRIPT_FUNCTION_END
	SCRIPT_END
	if(group.size()==0)
		Script::ClearPersistentObject<Group,LUAGroup>(_pState,group);
}

void Server::onManage(Client& client) {
	SCRIPT_BEGIN(client.object<Service>()->open())
		SCRIPT_MEMBER_FUNCTION_BEGIN(Client,LUAClient,client,"onManage")
			SCRIPT_FUNCTION_CALL
		SCRIPT_FUNCTION_END
	SCRIPT_END
}

