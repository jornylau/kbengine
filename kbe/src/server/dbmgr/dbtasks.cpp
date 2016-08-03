/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/

Copyright (c) 2008-2016 KBEngine.

KBEngine is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

KBEngine is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.
 
You should have received a copy of the GNU Lesser General Public License
along with KBEngine.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "dbtasks.h"
#include "dbmgr.h"
#include "buffered_dbtasks.h"
#include "network/common.h"
#include "network/message_handler.h"
#include "thread/threadpool.h"
#include "server/components.h"
#include "server/serverconfig.h"
#include "db_interface/db_interface.h"
#include "db_interface/kbe_tables.h"
#include "db_mysql/db_exception.h"
#include "db_mysql/db_interface_mysql.h"
#include "entitydef/scriptdef_module.h"
#include "openssl/md5.h"

#include "baseapp/baseapp_interface.h"
#include "cellapp/cellapp_interface.h"
#include "baseappmgr/baseappmgr_interface.h"
#include "cellappmgr/cellappmgr_interface.h"
#include "loginapp/loginapp_interface.h"

#if KBE_PLATFORM == PLATFORM_WIN32
#ifdef _DEBUG
#pragma comment(lib, "libeay32_d.lib")
#pragma comment(lib, "ssleay32_d.lib")
#else
#pragma comment(lib, "libeay32.lib")
#pragma comment(lib, "ssleay32.lib")
#endif
#endif


namespace KBEngine{

//-------------------------------------------------------------------------------------
DBTask::DBTask(const Network::Address& addr, MemoryStream& datas):
DBTaskBase(),
pDatas_(0),
addr_(addr)
{
	pDatas_ = MemoryStream::createPoolObject();
	*pDatas_ = datas;
}

//-------------------------------------------------------------------------------------
DBTask::~DBTask()
{
	if(pDatas_)
		MemoryStream::reclaimPoolObject(pDatas_);
}

//-------------------------------------------------------------------------------------
bool DBTask::send(Network::Bundle* pBundle)
{
	Network::Channel* pChannel = Dbmgr::getSingleton().networkInterface().findChannel(addr_);
	
	if(pChannel){
		pChannel->send(pBundle);
	}
	else{
		return false;
	}

	return true;
}

//-------------------------------------------------------------------------------------
DBTask* EntityDBTask::tryGetNextTask()
{
	KBE_ASSERT(_pBuffered_DBTasks != NULL);
	return _pBuffered_DBTasks->tryGetNextTask(this);
}

//-------------------------------------------------------------------------------------
thread::TPTask::TPTaskState EntityDBTask::presentMainThread()
{
	return DBTask::presentMainThread();
}

//-------------------------------------------------------------------------------------
DBTaskExecuteRawDatabaseCommand::DBTaskExecuteRawDatabaseCommand(const Network::Address& addr, MemoryStream& datas):
DBTask(addr, datas),
componentID_(0),
componentType_(UNKNOWN_COMPONENT_TYPE),
sdatas_(),
callbackID_(0),
error_(),
execret_()
{
}

//-------------------------------------------------------------------------------------
DBTaskExecuteRawDatabaseCommand::~DBTaskExecuteRawDatabaseCommand()
{
}

//-------------------------------------------------------------------------------------
bool DBTaskExecuteRawDatabaseCommand::db_thread_process()
{
	(*pDatas_) >> componentID_ >> componentType_;
	(*pDatas_) >> callbackID_;
	(*pDatas_).readBlob(sdatas_);

	try
	{
		if (!pdbi_->query(sdatas_.data(), (uint32)sdatas_.size(), false, &execret_))
		{
			error_ = pdbi_->getstrerror();
		}
	}
	catch (std::exception & e)
	{
		DBException& dbe = static_cast<DBException&>(e);
		if(dbe.isLostConnection())
		{
			static_cast<DBInterfaceMysql*>(pdbi_)->processException(e);
			return true;
		}

		error_ = e.what();
	}

	return false;
}

//-------------------------------------------------------------------------------------
DBTaskExecuteRawDatabaseCommandByEntity::DBTaskExecuteRawDatabaseCommandByEntity(const Network::Address& addr, MemoryStream& datas, ENTITY_ID entityID):
EntityDBTask(addr, datas, entityID, 0),
componentID_(0),
componentType_(UNKNOWN_COMPONENT_TYPE),
sdatas_(),
callbackID_(0),
error_(),
execret_()
{
}

//-------------------------------------------------------------------------------------
DBTaskExecuteRawDatabaseCommandByEntity::~DBTaskExecuteRawDatabaseCommandByEntity()
{
}

//-------------------------------------------------------------------------------------
bool DBTaskExecuteRawDatabaseCommandByEntity::db_thread_process()
{
	(*pDatas_) >> componentID_ >> componentType_;
	(*pDatas_) >> callbackID_;
	(*pDatas_).readBlob(sdatas_);

	try
	{
		if (!pdbi_->query(sdatas_.data(), (uint32)sdatas_.size(), false, &execret_))
		{
			error_ = pdbi_->getstrerror();
		}
	}
	catch (std::exception & e)
	{
		DBException& dbe = static_cast<DBException&>(e);
		if(dbe.isLostConnection())
		{
			static_cast<DBInterfaceMysql*>(pdbi_)->processException(e);
			return true;
		}

		error_ = e.what();
	}

	return false;
}

//-------------------------------------------------------------------------------------
thread::TPTask::TPTaskState DBTaskExecuteRawDatabaseCommandByEntity::presentMainThread()
{
	DEBUG_MSG(fmt::format("DBTask::ExecuteRawDatabaseCommandByEntity::presentMainThread: {}.\n", sdatas_.c_str()));

	// 如果不需要回调则结束
	if(callbackID_ <= 0)
		return EntityDBTask::presentMainThread();

	Network::Bundle* pBundle = Network::Bundle::createPoolObject();

	if(componentType_ == BASEAPP_TYPE)
		(*pBundle).newMessage(BaseappInterface::onExecuteRawDatabaseCommandCB);
	else if(componentType_ == CELLAPP_TYPE)
		(*pBundle).newMessage(CellappInterface::onExecuteRawDatabaseCommandCB);
	else
	{
		KBE_ASSERT(false && "no support!\n");
	}

	(*pBundle) << callbackID_;
	(*pBundle) << error_;

	if(error_.size() <= 0)
		(*pBundle).append(execret_);

	Components::ComponentInfos* cinfos = Components::getSingleton().findComponent(componentType_, componentID_);

	if(cinfos && cinfos->pChannel)
	{
		cinfos->pChannel->send(pBundle);
	}
	else
	{
		ERROR_MSG(fmt::format("DBTask::ExecuteRawDatabaseCommandByEntity::presentMainThread: {} not found!\n",
			COMPONENT_NAME_EX(componentType_)));

		Network::Bundle::reclaimPoolObject(pBundle);
	}

	return EntityDBTask::presentMainThread();
}

//-------------------------------------------------------------------------------------
thread::TPTask::TPTaskState DBTaskExecuteRawDatabaseCommand::presentMainThread()
{
	DEBUG_MSG(fmt::format("DBTask::ExecuteRawDatabaseCommandByEntity::presentMainThread: {}.\n", sdatas_.c_str()));

	// 如果不需要回调则结束
	if(callbackID_ <= 0)
		return thread::TPTask::TPTASK_STATE_COMPLETED;

	Network::Bundle* pBundle = Network::Bundle::createPoolObject();

	if(componentType_ == BASEAPP_TYPE)
		(*pBundle).newMessage(BaseappInterface::onExecuteRawDatabaseCommandCB);
	else if(componentType_ == CELLAPP_TYPE)
		(*pBundle).newMessage(CellappInterface::onExecuteRawDatabaseCommandCB);
	else
	{
		KBE_ASSERT(false && "no support!\n");
	}

	(*pBundle) << callbackID_;
	(*pBundle) << error_;

	if(error_.size() <= 0)
		(*pBundle).append(execret_);

	Components::ComponentInfos* cinfos = Components::getSingleton().findComponent(componentType_, componentID_);

	if(cinfos && cinfos->pChannel)
	{
		cinfos->pChannel->send(pBundle);
	}
	else
	{
		ERROR_MSG(fmt::format("DBTask::ExecuteRawDatabaseCommandByEntity::presentMainThread: {} not found!\n",
			COMPONENT_NAME_EX(componentType_)));

		Network::Bundle::reclaimPoolObject(pBundle);
	}

	return thread::TPTask::TPTASK_STATE_COMPLETED;
}

//-------------------------------------------------------------------------------------
DBTaskWriteEntity::DBTaskWriteEntity(const Network::Address& addr, 
									 COMPONENT_ID componentID, ENTITY_ID eid, 
									 DBID entityDBID, MemoryStream& datas):
EntityDBTask(addr, datas, eid, entityDBID),
componentID_(componentID),
eid_(eid),
entityDBID_(entityDBID),
sid_(0),
callbackID_(0),
shouldAutoLoad_(-1),
success_(false)
{
}

//-------------------------------------------------------------------------------------
DBTaskWriteEntity::~DBTaskWriteEntity()
{
}

//-------------------------------------------------------------------------------------
bool DBTaskWriteEntity::db_thread_process()
{
	(*pDatas_) >> sid_ >> callbackID_ >> shouldAutoLoad_;

	ScriptDefModule* pModule = EntityDef::findScriptModule(sid_);
	bool writeEntityLog = (entityDBID_ == 0);

	uint32 ip = 0;
	uint16 port = 0;

	if(writeEntityLog)
	{
		(*pDatas_) >> ip >> port;
	}

	EntityTables& entityTables = EntityTables::findByInterfaceName(pdbi_->name());

	entityDBID_ = entityTables.writeEntity(pdbi_, entityDBID_, shouldAutoLoad_, pDatas_, pModule);
	success_ = entityDBID_ > 0;

	if(writeEntityLog && success_)
	{
		success_ = false;

		// 先写log， 如果写失败则可能这个entity已经在线
		KBEEntityLogTable* pELTable = static_cast<KBEEntityLogTable*>(entityTables.findKBETable("kbe_entitylog"));
		KBE_ASSERT(pELTable);

		success_ = pELTable->logEntity(pdbi_, inet_ntoa((struct in_addr&)ip), port, entityDBID_, 
			componentID_, eid_, pModule->getUType());

		if(!success_)
		{
			entityDBID_ = 0;
		}
	}

	return false;
}

//-------------------------------------------------------------------------------------
thread::TPTask::TPTaskState DBTaskWriteEntity::presentMainThread()
{
	ScriptDefModule* pModule = EntityDef::findScriptModule(sid_);
	DEBUG_MSG(fmt::format("Dbmgr::writeEntity: {0}({1}).\n", pModule->getName(), entityDBID_));

	// 返回写entity的结果， 成功或者失败

	Network::Bundle* pBundle = Network::Bundle::createPoolObject();
	(*pBundle).newMessage(BaseappInterface::onWriteToDBCallback);
	BaseappInterface::onWriteToDBCallbackArgs5::staticAddToBundle((*pBundle), 
		eid_, entityDBID_, pdbi_->dbIndex(), callbackID_, success_);

	if(!this->send(pBundle))
	{
		ERROR_MSG(fmt::format("DBTaskWriteEntity::presentMainThread: channel({0}) not found.\n", addr_.c_str()));
		Network::Bundle::reclaimPoolObject(pBundle);
	}
	
	return EntityDBTask::presentMainThread();
}

//-------------------------------------------------------------------------------------
DBTaskRemoveEntity::DBTaskRemoveEntity(const Network::Address& addr, 
									 COMPONENT_ID componentID, ENTITY_ID eid, 
									 DBID entityDBID, MemoryStream& datas):
EntityDBTask(addr, datas, eid, entityDBID),
componentID_(componentID),
eid_(eid),
entityDBID_(entityDBID),
sid_(0)
{
}

//-------------------------------------------------------------------------------------
DBTaskRemoveEntity::~DBTaskRemoveEntity()
{
}

//-------------------------------------------------------------------------------------
bool DBTaskRemoveEntity::db_thread_process()
{
	(*pDatas_) >> sid_;

	EntityTables& entityTables = EntityTables::findByInterfaceName(pdbi_->name());
	KBEEntityLogTable* pELTable = static_cast<KBEEntityLogTable*>(entityTables.findKBETable("kbe_entitylog"));

	KBE_ASSERT(pELTable);
	pELTable->eraseEntityLog(pdbi_, entityDBID_, sid_);

	entityTables.removeEntity(pdbi_, entityDBID_, EntityDef::findScriptModule(sid_));
	return false;
}

//-------------------------------------------------------------------------------------
thread::TPTask::TPTaskState DBTaskRemoveEntity::presentMainThread()
{
	ScriptDefModule* pModule = EntityDef::findScriptModule(sid_);
	DEBUG_MSG(fmt::format("Dbmgr::removeEntity: {}({}).\n", pModule->getName(), entityDBID_));
	return EntityDBTask::presentMainThread();
}

//-------------------------------------------------------------------------------------
DBTaskDeleteBaseByDBID::DBTaskDeleteBaseByDBID(const Network::Address& addr, COMPONENT_ID componentID, 
		DBID entityDBID, CALLBACK_ID callbackID, ENTITY_SCRIPT_UID sid):
DBTask(addr),
componentID_(componentID),
callbackID_(callbackID),
entityDBID_(entityDBID),
sid_(sid),
success_(false),
entityID_(0),
entityInAppID_(0)
{
}

//-------------------------------------------------------------------------------------
DBTaskDeleteBaseByDBID::~DBTaskDeleteBaseByDBID()
{
}

//-------------------------------------------------------------------------------------
bool DBTaskDeleteBaseByDBID::db_thread_process()
{
	EntityTables& entityTables = EntityTables::findByInterfaceName(pdbi_->name());
	KBEEntityLogTable* pELTable = static_cast<KBEEntityLogTable*>
		(entityTables.findKBETable("kbe_entitylog"));

	KBE_ASSERT(pELTable);

	bool haslog = false;
	KBEEntityLogTable::EntityLog entitylog;

	ScriptDefModule* pModule = EntityDef::findScriptModule(sid_);

	haslog = pELTable->queryEntity(pdbi_, entityDBID_, entitylog, pModule->getUType());

	// 如果有在线纪录
	if(haslog)
	{
		success_ = false;
		entityInAppID_ = entitylog.componentID;
		entityID_ = entitylog.entityID;
		KBE_ASSERT(entityID_ > 0 && entityInAppID_ > 0);
		return false;
	}

	entityTables.removeEntity(pdbi_, entityDBID_, pModule);
	success_ = true;
	return false;
}

//-------------------------------------------------------------------------------------
thread::TPTask::TPTaskState DBTaskDeleteBaseByDBID::presentMainThread()
{
	ScriptDefModule* pModule = EntityDef::findScriptModule(sid_);
	DEBUG_MSG(fmt::format("Dbmgr::DBTaskDeleteBaseByDBID: {}({}), entityInAppID({}).\n", 
		pModule->getName(), entityDBID_, entityInAppID_));

	Network::Bundle* pBundle = Network::Bundle::createPoolObject();
	(*pBundle).newMessage(BaseappInterface::deleteBaseByDBIDCB);

	(*pBundle) << success_ << entityID_ << entityInAppID_ << callbackID_ << sid_ << entityDBID_;

	if(!this->send(pBundle))
	{
		ERROR_MSG(fmt::format("DBTaskDeleteBaseByDBID::presentMainThread: channel({}) not found.\n", addr_.c_str()));
		Network::Bundle::reclaimPoolObject(pBundle);
	}

	return thread::TPTask::TPTASK_STATE_COMPLETED;
}

//-------------------------------------------------------------------------------------
DBTaskEntityAutoLoad::DBTaskEntityAutoLoad(const Network::Address& addr, COMPONENT_ID componentID, 
		ENTITY_SCRIPT_UID entityType, ENTITY_ID start, ENTITY_ID end):
DBTask(addr),
componentID_(componentID),
entityType_(entityType),
start_(start),
end_(end),
outs_()
{
}

//-------------------------------------------------------------------------------------
DBTaskEntityAutoLoad::~DBTaskEntityAutoLoad()
{
}

//-------------------------------------------------------------------------------------
bool DBTaskEntityAutoLoad::db_thread_process()
{
	ScriptDefModule* pModule = EntityDef::findScriptModule(entityType_);
	EntityTables::findByInterfaceName(pdbi_->name()).queryAutoLoadEntities(this->pdbi_, pModule, start_, end_, outs_);
	return false;
}

//-------------------------------------------------------------------------------------
thread::TPTask::TPTaskState DBTaskEntityAutoLoad::presentMainThread()
{
	int size = (int)outs_.size();
	ScriptDefModule* pModule = EntityDef::findScriptModule(entityType_);

	if(size > 0)
	{
		DEBUG_MSG(fmt::format("Dbmgr::DBTaskEntityAutoLoad: {}, size({}).\n", 
			pModule->getName(), size));
	}

	Network::Bundle* pBundle = Network::Bundle::createPoolObject();
	(*pBundle).newMessage(BaseappInterface::onEntityAutoLoadCBFromDBMgr);

	(*pBundle) << pdbi_->dbIndex() << size << entityType_;

	std::vector<DBID>::iterator iter = outs_.begin();
	for(; iter != outs_.end(); iter++)
	{
		(*pBundle) << (*iter);
	}

	if(!this->send(pBundle))
	{
		ERROR_MSG(fmt::format("DBTaskEntityAutoLoad::presentMainThread: channel({}) not found.\n", addr_.c_str()));
		Network::Bundle::reclaimPoolObject(pBundle);
	}

	return thread::TPTask::TPTASK_STATE_COMPLETED;
}

//-------------------------------------------------------------------------------------
DBTaskLookUpBaseByDBID::DBTaskLookUpBaseByDBID(const Network::Address& addr, COMPONENT_ID componentID, 
		DBID entityDBID, CALLBACK_ID callbackID, ENTITY_SCRIPT_UID sid):
DBTask(addr),
componentID_(componentID),
callbackID_(callbackID),
entityDBID_(entityDBID),
sid_(sid),
success_(false),
entityID_(0),
entityInAppID_(0)
{
}

//-------------------------------------------------------------------------------------
DBTaskLookUpBaseByDBID::~DBTaskLookUpBaseByDBID()
{
}

//-------------------------------------------------------------------------------------
bool DBTaskLookUpBaseByDBID::db_thread_process()
{
	EntityTables& entityTables = EntityTables::findByInterfaceName(pdbi_->name());
	KBEEntityLogTable* pELTable = static_cast<KBEEntityLogTable*>
		(entityTables.findKBETable("kbe_entitylog"));

	KBE_ASSERT(pELTable);

	KBEEntityLogTable::EntityLog entitylog;

	ScriptDefModule* pModule = EntityDef::findScriptModule(sid_);

	// 如果有在线纪录
	if(pELTable->queryEntity(pdbi_, entityDBID_, entitylog, pModule->getUType()))
	{
		success_ = true;
		entityInAppID_ = entitylog.componentID;
		entityID_ = entitylog.entityID;
		KBE_ASSERT(entityID_ > 0 && entityInAppID_ > 0);
		return false;
	}

	MemoryStream s;
	success_ = entityTables.queryEntity(pdbi_, entityDBID_, &s, pModule);
	return false;
}

//-------------------------------------------------------------------------------------
thread::TPTask::TPTaskState DBTaskLookUpBaseByDBID::presentMainThread()
{
	ScriptDefModule* pModule = EntityDef::findScriptModule(sid_);
	DEBUG_MSG(fmt::format("Dbmgr::DBTaskLookUpBaseByDBID: {}({}), entityInAppID({}).\n", 
		pModule->getName(), entityDBID_, entityInAppID_));

	Network::Bundle* pBundle = Network::Bundle::createPoolObject();
	(*pBundle).newMessage(BaseappInterface::lookUpBaseByDBIDCB);

	(*pBundle) << success_ << entityID_ << entityInAppID_ << callbackID_ << sid_ << entityDBID_;

	if(!this->send(pBundle))
	{
		ERROR_MSG(fmt::format("DBTaskLookUpBaseByDBID::presentMainThread: channel({}) not found.\n", addr_.c_str()));
		Network::Bundle::reclaimPoolObject(pBundle);
	}

	return thread::TPTask::TPTASK_STATE_COMPLETED;
}

//-------------------------------------------------------------------------------------
DBTaskCreateAccount::DBTaskCreateAccount(const Network::Address& addr, 
										 std::string& registerName,
										 std::string& accountName, 
										 std::string& password, 
										 std::string& postdatas, 
										std::string& getdatas):
DBTask(addr),
registerName_(registerName),
accountName_(accountName),
password_(password),
postdatas_(postdatas),
getdatas_(getdatas),
success_(false)
{
}

//-------------------------------------------------------------------------------------
DBTaskCreateAccount::~DBTaskCreateAccount()
{
}

//-------------------------------------------------------------------------------------
bool DBTaskCreateAccount::db_thread_process()
{
	ACCOUNT_INFOS info;
	success_ = DBTaskCreateAccount::writeAccount(pdbi_, accountName_, password_, postdatas_, info) && info.dbid > 0;
	return false;
}

//-------------------------------------------------------------------------------------
bool DBTaskCreateAccount::writeAccount(DBInterface* pdbi, const std::string& accountName, 
									   const std::string& passwd, const std::string& datas, ACCOUNT_INFOS& info)
{
	info.dbid = 0;
	if(accountName.size() == 0)
	{
		return false;
	}

	// 寻找dblog是否有此账号， 如果有则创建失败
	// 如果没有则向account表新建一个entity数据同时在accountlog表写入一个log关联dbid
	EntityTables& entityTables = EntityTables::findByInterfaceName(pdbi->name());
	KBEAccountTable* pTable = static_cast<KBEAccountTable*>(entityTables.findKBETable("kbe_accountinfos"));
	KBE_ASSERT(pTable);

	ScriptDefModule* pModule = EntityDef::findScriptModule(DBUtil::accountScriptName());
	if(pModule == NULL)
	{
		ERROR_MSG(fmt::format("DBTaskCreateAccount::writeAccount(): not found account script[{}], create[{}] error!\n", 
			DBUtil::accountScriptName(), accountName));

		return false;
	}

	if(pTable->queryAccount(pdbi, accountName, info) && (info.flags & ACCOUNT_FLAG_NOT_ACTIVATED) <= 0)
	{
		if(pdbi->getlasterror() > 0)
		{
			WARNING_MSG(fmt::format("DBTaskCreateAccount::writeAccount(): queryAccount error: {}\n", 
				pdbi->getstrerror()));
		}

		return false;
	}

	bool hasset = (info.dbid != 0);
	if(!hasset)
	{
		info.flags = g_kbeSrvConfig.getDBMgr().accountDefaultFlags;
		info.deadline = g_kbeSrvConfig.getDBMgr().accountDefaultDeadline;
	}

	DBID entityDBID = info.dbid;
	
	if(entityDBID == 0)
	{
		// 防止多线程问题， 这里做一个拷贝。
		MemoryStream copyAccountDefMemoryStream(pTable->accountDefMemoryStream());

		entityDBID = EntityTables::findByInterfaceName(pdbi->name()).writeEntity(pdbi, 0, -1,
				&copyAccountDefMemoryStream, pModule);
	}

	KBE_ASSERT(entityDBID > 0);

	info.name = accountName;
	info.email = accountName + "@0.0";
	info.password = passwd;
	info.dbid = entityDBID;
	info.datas = datas;
	
	if(!hasset)
	{
		if(!pTable->logAccount(pdbi, info))
		{
			if(pdbi->getlasterror() > 0)
			{
				WARNING_MSG(fmt::format("DBTaskCreateAccount::writeAccount(): logAccount error:{}\n", 
					pdbi->getstrerror()));
			}

			return false;
		}
	}
	else
	{
		if(!pTable->setFlagsDeadline(pdbi, accountName, info.flags & ~ACCOUNT_FLAG_NOT_ACTIVATED, info.deadline))
		{
			if(pdbi->getlasterror() > 0)
			{
				WARNING_MSG(fmt::format("DBTaskCreateAccount::writeAccount(): logAccount error:{}\n", 
					pdbi->getstrerror()));
			}

			return false;
		}
	}

	return true;
}

//-------------------------------------------------------------------------------------
thread::TPTask::TPTaskState DBTaskCreateAccount::presentMainThread()
{
	DEBUG_MSG(fmt::format("Dbmgr::reqCreateAccount: {}.\n", registerName_.c_str()));

	Network::Bundle* pBundle = Network::Bundle::createPoolObject();
	(*pBundle).newMessage(LoginappInterface::onReqCreateAccountResult);
	SERVER_ERROR_CODE failedcode = SERVER_SUCCESS;

	if(!success_)
		failedcode = SERVER_ERR_ACCOUNT_CREATE_FAILED;

	(*pBundle) << failedcode << registerName_ << password_;
	(*pBundle).appendBlob(getdatas_);

	if(!this->send(pBundle))
	{
		ERROR_MSG(fmt::format("DBTaskCreateAccount::presentMainThread: channel({}) not found.\n", addr_.c_str()));
		Network::Bundle::reclaimPoolObject(pBundle);
	}

	return thread::TPTask::TPTASK_STATE_COMPLETED;
}

//-------------------------------------------------------------------------------------
std::string genmail_code(const std::string& str)
{
	std::string datas = KBEngine::StringConv::val2str(KBEngine::genUUID64());
	datas += KBE_MD5::getDigest(str.data(), (int)str.length());

	srand(getSystemTime());
	datas += KBEngine::StringConv::val2str(rand());
	return datas;
}

//-------------------------------------------------------------------------------------
DBTaskCreateMailAccount::DBTaskCreateMailAccount(const Network::Address& addr, 
										 std::string& registerName,
										 std::string& accountName, 
										 std::string& password, 
										 std::string& postdatas, 
										std::string& getdatas):
DBTask(addr),
registerName_(registerName),
accountName_(accountName),
password_(password),
postdatas_(postdatas),
getdatas_(getdatas),
success_(false)
{
}

//-------------------------------------------------------------------------------------
DBTaskCreateMailAccount::~DBTaskCreateMailAccount()
{
}

//-------------------------------------------------------------------------------------
bool DBTaskCreateMailAccount::db_thread_process()
{
	ACCOUNT_INFOS info;
	if(accountName_.size() == 0)
	{
		ERROR_MSG("DBTaskCreateMailAccount::db_thread_process(): accountName is NULL!\n");
		return false;
	}

	// 寻找dblog是否有此账号， 如果有则创建失败
	EntityTables& entityTables = EntityTables::findByInterfaceName(pdbi_->name());
	KBEAccountTable* pTable = static_cast<KBEAccountTable*>(entityTables.findKBETable("kbe_accountinfos"));
	KBE_ASSERT(pTable);
	
	info.flags = 0;
	if(pTable->queryAccount(pdbi_, accountName_, info)  && (info.flags & ACCOUNT_FLAG_NOT_ACTIVATED) <= 0)
	{
		if(pdbi_->getlasterror() > 0)
		{
			WARNING_MSG(fmt::format("DBTaskCreateMailAccount::db_thread_process(): queryAccount is error: {}\n", 
				pdbi_->getstrerror()));
		}

		return false;
	}

	// 生成激活码并存储激活码到数据库
	// 发送smtp邮件到邮箱， 用户点击确认后即可激活
	getdatas_ = genmail_code(password_);
	KBEEmailVerificationTable* pTable1 = static_cast<KBEEmailVerificationTable*>(entityTables.findKBETable("kbe_email_verification"));
	KBE_ASSERT(pTable1);
	
	info.datas = getdatas_;
	info.name = registerName_;
	info.password = password_;
	info.flags |= ACCOUNT_FLAG_NOT_ACTIVATED; 
	info.email = info.name;
	info.dbid = KBEngine::genUUID64();

	try
	{
		pTable->logAccount(pdbi_, info);
	}
	catch (...)
	{
		WARNING_MSG(fmt::format("DBTaskCreateMailAccount::db_thread_process(): logAccount(kbe_accountinfos) is error: {}\n{}\n", 
			pdbi_->getstrerror(), pdbi_->lastquery()));
	}

	password_ = KBE_MD5::getDigest(password_.data(), (int)password_.length());

	success_ = pTable1->logAccount(pdbi_, (int8)KBEEmailVerificationTable::V_TYPE_CREATEACCOUNT, 
		registerName_, password_, getdatas_);

	return false;
}

//-------------------------------------------------------------------------------------
thread::TPTask::TPTaskState DBTaskCreateMailAccount::presentMainThread()
{
	DEBUG_MSG(fmt::format("Dbmgr::reqCreateMailAccount: {}, success={}.\n", registerName_, success_));

	Network::Bundle* pBundle = Network::Bundle::createPoolObject();
	(*pBundle).newMessage(LoginappInterface::onReqCreateMailAccountResult);
	SERVER_ERROR_CODE failedcode = SERVER_SUCCESS;

	if(!success_)
		failedcode = SERVER_ERR_ACCOUNT_CREATE_FAILED;

	(*pBundle) << failedcode << registerName_ << password_;
	(*pBundle).appendBlob(getdatas_);

	if(!this->send(pBundle))
	{
		ERROR_MSG(fmt::format("DBTaskCreateMailAccount::presentMainThread: channel({}) not found.\n", addr_.c_str()));
		Network::Bundle::reclaimPoolObject(pBundle);
	}

	return thread::TPTask::TPTASK_STATE_COMPLETED;
}

//-------------------------------------------------------------------------------------
DBTaskActivateAccount::DBTaskActivateAccount(const Network::Address& addr, 
										 std::string& code):
DBTask(addr),
code_(code),
success_(false)
{
}

//-------------------------------------------------------------------------------------
DBTaskActivateAccount::~DBTaskActivateAccount()
{
}

//-------------------------------------------------------------------------------------
bool DBTaskActivateAccount::db_thread_process()
{
	ACCOUNT_INFOS info;

	KBEEmailVerificationTable* pTable1 = 
		static_cast<KBEEmailVerificationTable*>(EntityTables::findByInterfaceName(pdbi_->name()).findKBETable("kbe_email_verification"));

	KBE_ASSERT(pTable1);

	success_ = pTable1->activateAccount(pdbi_, code_, info);
	if(!success_)
	{
		ERROR_MSG(fmt::format("DBTaskActivateAccount::db_thread_process(): activateAccount({1}) error: {0}\n", 
				pdbi_->getstrerror(), code_));
		return false;
	}

	return false;
}

//-------------------------------------------------------------------------------------
thread::TPTask::TPTaskState DBTaskActivateAccount::presentMainThread()
{
	Network::Bundle* pBundle = Network::Bundle::createPoolObject();

	(*pBundle).newMessage(LoginappInterface::onAccountActivated);
	(*pBundle) << code_ << success_;

	if(!this->send(pBundle))
	{
		ERROR_MSG(fmt::format("DBTaskActivateAccount::presentMainThread: channel({}) not found.\n", addr_.c_str()));
		Network::Bundle::reclaimPoolObject(pBundle);
	}

	return thread::TPTask::TPTASK_STATE_COMPLETED;
}


//-------------------------------------------------------------------------------------
DBTaskReqAccountResetPassword::DBTaskReqAccountResetPassword(const Network::Address& addr, std::string& accountName):
DBTask(addr),
code_(),
email_(),
accountName_(accountName),
success_(false)
{
}

//-------------------------------------------------------------------------------------
DBTaskReqAccountResetPassword::~DBTaskReqAccountResetPassword()
{
}

//-------------------------------------------------------------------------------------
bool DBTaskReqAccountResetPassword::db_thread_process()
{
	ACCOUNT_INFOS info;
	if(accountName_.size() == 0)
	{
		return false;
	}

	EntityTables& entityTables = EntityTables::findByInterfaceName(pdbi_->name());
	KBEAccountTable* pTable = static_cast<KBEAccountTable*>(entityTables.findKBETable("kbe_accountinfos"));
	KBE_ASSERT(pTable);

	if(!pTable->queryAccountAllInfos(pdbi_, accountName_, info))
	{
		return false;
	}

	if(info.dbid == 0 || info.flags != ACCOUNT_FLAG_NORMAL)
		return false;
	
	// 生成激活码并存储激活码到数据库
	// 发送smtp邮件到邮箱， 用户点击确认后即可激活
	KBEEmailVerificationTable* pTable1 = 
		static_cast<KBEEmailVerificationTable*>(entityTables.findKBETable("kbe_email_verification"));
	KBE_ASSERT(pTable1);

	info.datas = genmail_code(accountName_);
	code_ = info.datas;
	email_ = info.email;
	success_ = pTable1->logAccount(pdbi_, (int8)KBEEmailVerificationTable::V_TYPE_RESETPASSWORD, accountName_, email_, code_);
	return false;
}

//-------------------------------------------------------------------------------------
thread::TPTask::TPTaskState DBTaskReqAccountResetPassword::presentMainThread()
{
	DEBUG_MSG(fmt::format("DBTaskReqAccountResetPassword::presentMainThread: accountName={}, code_={}, success={}.\n",
		accountName_, code_, success_));

	Network::Bundle* pBundle = Network::Bundle::createPoolObject();
	(*pBundle).newMessage(LoginappInterface::onReqAccountResetPasswordCB);
	SERVER_ERROR_CODE failedcode = SERVER_SUCCESS;

	if(!success_)
		failedcode = SERVER_ERR_OP_FAILED;

	(*pBundle) << accountName_;
	(*pBundle) << email_;
	(*pBundle) << failedcode;
	(*pBundle) << code_;

	if(!this->send(pBundle))
	{
		ERROR_MSG(fmt::format("DBTaskReqAccountResetPassword::presentMainThread: channel({}) not found.\n", addr_.c_str()));
		Network::Bundle::reclaimPoolObject(pBundle);
	}

	return thread::TPTask::TPTASK_STATE_COMPLETED;
}

//-------------------------------------------------------------------------------------
DBTaskAccountResetPassword::DBTaskAccountResetPassword(const Network::Address& addr, std::string& accountName, 
		std::string& newpassword, std::string& code):
DBTask(addr),
code_(code),
accountName_(accountName),
newpassword_(newpassword),
success_(false)
{
}

//-------------------------------------------------------------------------------------
DBTaskAccountResetPassword::~DBTaskAccountResetPassword()
{
}

//-------------------------------------------------------------------------------------
bool DBTaskAccountResetPassword::db_thread_process()
{
	KBEEmailVerificationTable* pTable1 = static_cast<KBEEmailVerificationTable*>(
		EntityTables::findByInterfaceName(pdbi_->name()).findKBETable("kbe_email_verification"));

	KBE_ASSERT(pTable1);

	success_ = pTable1->resetpassword(pdbi_, accountName_, newpassword_, code_);
	return false;
}

//-------------------------------------------------------------------------------------
thread::TPTask::TPTaskState DBTaskAccountResetPassword::presentMainThread()
{
	DEBUG_MSG(fmt::format("DBTaskAccountResetPassword::presentMainThread: code({}), success={}.\n",
		code_, success_));

	Network::Bundle* pBundle = Network::Bundle::createPoolObject();
	(*pBundle).newMessage(LoginappInterface::onAccountResetPassword);

	(*pBundle) << code_;
	(*pBundle) << success_;

	if(!this->send(pBundle))
	{
		ERROR_MSG(fmt::format("DBTaskAccountResetPassword::presentMainThread: channel({}) not found.\n", addr_.c_str()));
		Network::Bundle::reclaimPoolObject(pBundle);
	}

	return thread::TPTask::TPTASK_STATE_COMPLETED;
}

//-------------------------------------------------------------------------------------
DBTaskReqAccountBindEmail::DBTaskReqAccountBindEmail(const Network::Address& addr, ENTITY_ID entityID, std::string& accountName, 
		std::string password,std::string& email):
DBTask(addr),
code_(),
password_(password),
accountName_(accountName),
email_(email),
success_(false),
entityID_(entityID)
{
}

//-------------------------------------------------------------------------------------
DBTaskReqAccountBindEmail::~DBTaskReqAccountBindEmail()
{
}

//-------------------------------------------------------------------------------------
bool DBTaskReqAccountBindEmail::db_thread_process()
{
	ACCOUNT_INFOS info;
	if(accountName_.size() == 0)
	{
		return false;
	}

	// 生成激活码并存储激活码到数据库
	// 发送smtp邮件到邮箱， 用户点击确认后即可激活
	KBEEmailVerificationTable* pTable1 = static_cast<KBEEmailVerificationTable*>(
		EntityTables::findByInterfaceName(pdbi_->name()).findKBETable("kbe_email_verification"));

	KBE_ASSERT(pTable1);

	info.datas = genmail_code(accountName_);
	info.name = accountName_;
	code_ = info.datas;
	success_ = pTable1->logAccount(pdbi_, (int8)KBEEmailVerificationTable::V_TYPE_BIND_MAIL, accountName_, email_, code_);
	return false;
}

//-------------------------------------------------------------------------------------
thread::TPTask::TPTaskState DBTaskReqAccountBindEmail::presentMainThread()
{
	DEBUG_MSG(fmt::format("DBTaskReqAccountBindEmail::presentMainThread: code({}), success={}.\n", 
		code_, success_));

	Network::Bundle* pBundle = Network::Bundle::createPoolObject();
	(*pBundle).newMessage(BaseappInterface::onReqAccountBindEmailCB);
	SERVER_ERROR_CODE failedcode = SERVER_SUCCESS;

	if(!success_)
		failedcode = SERVER_ERR_OP_FAILED;

	(*pBundle) << entityID_; 
	(*pBundle) << accountName_;
	(*pBundle) << email_;
	(*pBundle) << failedcode;
	(*pBundle) << code_;

	if(!this->send(pBundle))
	{
		ERROR_MSG(fmt::format("DBTaskReqAccountBindEmail::presentMainThread: channel({}) not found.\n", addr_.c_str()));
		Network::Bundle::reclaimPoolObject(pBundle);
	}

	return thread::TPTask::TPTASK_STATE_COMPLETED;
}

//-------------------------------------------------------------------------------------
DBTaskAccountBindEmail::DBTaskAccountBindEmail(const Network::Address& addr, std::string& accountName, 
		std::string& code):
DBTask(addr),
code_(code),
accountName_(accountName),
success_(false)
{
}

//-------------------------------------------------------------------------------------
DBTaskAccountBindEmail::~DBTaskAccountBindEmail()
{
}

//-------------------------------------------------------------------------------------
bool DBTaskAccountBindEmail::db_thread_process()
{
	KBEEmailVerificationTable* pTable1 = static_cast<KBEEmailVerificationTable*>(
		EntityTables::findByInterfaceName(pdbi_->name()).findKBETable("kbe_email_verification"));

	KBE_ASSERT(pTable1);

	success_ = pTable1->bindEMail(pdbi_, accountName_, code_);
	return false;
}

//-------------------------------------------------------------------------------------
thread::TPTask::TPTaskState DBTaskAccountBindEmail::presentMainThread()
{
	DEBUG_MSG(fmt::format("DBTaskAccountBindEmail::presentMainThread: code({}), success={}.\n", 
		code_, success_));

	Network::Bundle* pBundle = Network::Bundle::createPoolObject();
	(*pBundle).newMessage(LoginappInterface::onAccountBindedEmail);

	(*pBundle) << code_;
	(*pBundle) << success_;

	if(!this->send(pBundle))
	{
		ERROR_MSG(fmt::format("DBTaskAccountBindEmail::presentMainThread: channel({}) not found.\n", addr_.c_str()));
		Network::Bundle::reclaimPoolObject(pBundle);
	}

	return thread::TPTask::TPTASK_STATE_COMPLETED;
}

//-------------------------------------------------------------------------------------
DBTaskAccountNewPassword::DBTaskAccountNewPassword(const Network::Address& addr, ENTITY_ID entityID, std::string& accountName, 
		std::string& oldpassword_, std::string& newpassword):
DBTask(addr),
accountName_(accountName),
oldpassword_(oldpassword_), newpassword_(newpassword),
success_(false),
entityID_(entityID)
{
}

//-------------------------------------------------------------------------------------
DBTaskAccountNewPassword::~DBTaskAccountNewPassword()
{
}

//-------------------------------------------------------------------------------------
bool DBTaskAccountNewPassword::db_thread_process()
{
	KBEAccountTable* pTable = static_cast<KBEAccountTable*>(
		EntityTables::findByInterfaceName(pdbi_->name()).findKBETable("kbe_accountinfos"));

	KBE_ASSERT(pTable);

	ACCOUNT_INFOS info;
	if(!pTable->queryAccount(pdbi_, accountName_, info))
	{
		return false;
	}

	if(info.dbid == 0 || info.flags != ACCOUNT_FLAG_NORMAL)
		return false;

	if (kbe_stricmp(info.password.c_str(), KBE_MD5::getDigest(oldpassword_.data(), (int)oldpassword_.length()).c_str()) != 0)
	{
		return false;
	}

	success_ = pTable->updatePassword(pdbi_, accountName_, KBE_MD5::getDigest(newpassword_.data(), (int)newpassword_.length()));
	return false;
}

//-------------------------------------------------------------------------------------
thread::TPTask::TPTaskState DBTaskAccountNewPassword::presentMainThread()
{
	DEBUG_MSG(fmt::format("DBTaskAccountNewPassword::presentMainThread: success={}.\n", success_));

	Network::Bundle* pBundle = Network::Bundle::createPoolObject();
	(*pBundle).newMessage(BaseappInterface::onReqAccountNewPasswordCB);

	SERVER_ERROR_CODE failedcode = SERVER_SUCCESS;

	if(!success_)
		failedcode = SERVER_ERR_OP_FAILED;

	(*pBundle) << entityID_;
	(*pBundle) << accountName_;
	(*pBundle) << failedcode;

	if(!this->send(pBundle))
	{
		ERROR_MSG(fmt::format("DBTaskAccountNewPassword::presentMainThread: channel({}) not found.\n", addr_.c_str()));
		Network::Bundle::reclaimPoolObject(pBundle);
	}

	return thread::TPTask::TPTASK_STATE_COMPLETED;
}

//-------------------------------------------------------------------------------------
DBTaskQueryAccount::DBTaskQueryAccount(const Network::Address& addr, std::string& accountName, std::string& password, 
		COMPONENT_ID componentID, ENTITY_ID entityID, DBID entityDBID, uint32 ip, uint16 port):
EntityDBTask(addr, entityID, entityDBID),
accountName_(accountName),
password_(password),
success_(false),
s_(),
dbid_(entityDBID),
componentID_(componentID),
entityID_(entityID),
error_(),
ip_(ip),
port_(port),
flags_(0),
deadline_(0)
{
}

//-------------------------------------------------------------------------------------
DBTaskQueryAccount::~DBTaskQueryAccount()
{
}

//-------------------------------------------------------------------------------------
bool DBTaskQueryAccount::db_thread_process()
{
	if(accountName_.size() == 0)
	{
		error_ = "accountName_ is NULL";
		return false;
	}

	EntityTables& entityTables = EntityTables::findByInterfaceName(pdbi_->name());
	KBEAccountTable* pTable = static_cast<KBEAccountTable*>(entityTables.findKBETable("kbe_accountinfos"));
	KBE_ASSERT(pTable);

	ACCOUNT_INFOS info;
	info.name = "";
	info.password = "";
	info.dbid = dbid_;

	if(dbid_ == 0)
	{
		if(!pTable->queryAccount(pdbi_, accountName_, info))
		{
			error_ = "pTable->queryAccount() is failed!";
			
			if(pdbi_->getlasterror() > 0)
			{
				error_ += pdbi_->getstrerror();
			}
	
			return false;
		}

		if(info.dbid == 0 || info.flags != ACCOUNT_FLAG_NORMAL)
		{
			error_ = "dbid is 0 or flags != ACCOUNT_FLAG_NORMAL";
			return false;
		}

		if (kbe_stricmp(info.password.c_str(), KBE_MD5::getDigest(password_.data(), (int)password_.length()).c_str()) != 0)
		{
			error_ = "password is error";
			return false;
		}
	}

	ScriptDefModule* pModule = EntityDef::findScriptModule(DBUtil::accountScriptName());
	success_ = entityTables.queryEntity(pdbi_, info.dbid, &s_, pModule);

	if(!success_ && pdbi_->getlasterror() > 0)
	{
		error_ += "queryEntity: ";
		error_ += pdbi_->getstrerror();
	}

	dbid_ = info.dbid;

	if(!success_)
		return false;

	success_ = false;

	// 先写log， 如果写失败则可能这个entity已经在线
	KBEEntityLogTable* pELTable = static_cast<KBEEntityLogTable*>
		(entityTables.findKBETable("kbe_entitylog"));
	KBE_ASSERT(pELTable);
	
	success_ = pELTable->logEntity(pdbi_, inet_ntoa((struct in_addr&)ip_), port_, dbid_, 
		componentID_, entityID_, pModule->getUType());

	if(!success_ && pdbi_->getlasterror() > 0)
	{
		error_ += "logEntity: ";
		error_ += pdbi_->getstrerror();
	}

	flags_ = info.flags;
	deadline_ = info.deadline;

	return false;
}

//-------------------------------------------------------------------------------------
thread::TPTask::TPTaskState DBTaskQueryAccount::presentMainThread()
{
	DEBUG_MSG(fmt::format("Dbmgr::queryAccount: {}, success={}, flags={}, deadline={}.\n", 
		 accountName_.c_str(), success_, flags_, deadline_));

	Network::Bundle* pBundle = Network::Bundle::createPoolObject();
	(*pBundle).newMessage(BaseappInterface::onQueryAccountCBFromDbmgr);
	(*pBundle) << pdbi_->dbIndex();
	(*pBundle) << accountName_;
	(*pBundle) << password_;
	(*pBundle) << dbid_;
	(*pBundle) << success_;
	(*pBundle) << entityID_;
	(*pBundle) << flags_;
	(*pBundle) << deadline_;

	if(success_)
	{
		pBundle->append(s_);
	}
	else
	{
		(*pBundle) << error_;
	}

	if(!this->send(pBundle))
	{
		ERROR_MSG(fmt::format("DBTaskQueryAccount::presentMainThread: channel({}) not found.\n", addr_.c_str()));
		Network::Bundle::reclaimPoolObject(pBundle);
	}

	return EntityDBTask::presentMainThread();
}

//-------------------------------------------------------------------------------------
DBTaskAccountOnline::DBTaskAccountOnline(const Network::Address& addr, std::string& accountName,
		COMPONENT_ID componentID, ENTITY_ID entityID):
EntityDBTask(addr, entityID, 0),
accountName_(accountName),
componentID_(componentID)
{
}

//-------------------------------------------------------------------------------------
DBTaskAccountOnline::~DBTaskAccountOnline()
{
}

//-------------------------------------------------------------------------------------
bool DBTaskAccountOnline::db_thread_process()
{
	return false;
}

//-------------------------------------------------------------------------------------
thread::TPTask::TPTaskState DBTaskAccountOnline::presentMainThread()
{
	DEBUG_MSG(fmt::format("Dbmgr::onAccountOnline: componentID:{}, entityID:{}.\n", componentID_, EntityDBTask_entityID()));

	/*
	// 如果没有连接db则从log中查找账号是否还在线
	if(!pDBInterface_)
	{
		PROXICES_ONLINE_LOG::iterator iter = proxicesOnlineLogs_.find(accountName_);
		if(iter != proxicesOnlineLogs_.end())
		{
			iter->second.cid = componentID_;
			iter->second.eid = entityID_;
		}
		else
		{
			proxicesOnlineLogs_[accountName].cid = componentID_;
			proxicesOnlineLogs_[accountName].eid = entityID_;
		}
	}
	*/

	return EntityDBTask::presentMainThread();
}

//-------------------------------------------------------------------------------------
DBTaskEntityOffline::DBTaskEntityOffline(const Network::Address& addr, DBID dbid, ENTITY_SCRIPT_UID sid):
EntityDBTask(addr, 0, dbid),
sid_(sid)
{
}

//-------------------------------------------------------------------------------------
DBTaskEntityOffline::~DBTaskEntityOffline()
{
}

//-------------------------------------------------------------------------------------
bool DBTaskEntityOffline::db_thread_process()
{
	KBEEntityLogTable* pELTable = static_cast<KBEEntityLogTable*>
		(EntityTables::findByInterfaceName(pdbi_->name()).findKBETable("kbe_entitylog"));

	KBE_ASSERT(pELTable);

	pELTable->eraseEntityLog(pdbi_, EntityDBTask_entityDBID(), sid_);
	return false;
}

//-------------------------------------------------------------------------------------
thread::TPTask::TPTaskState DBTaskEntityOffline::presentMainThread()
{
	DEBUG_MSG(fmt::format("Dbmgr::onEntityOffline: {}, entityType={}.\n", EntityDBTask_entityDBID(), sid_));
	return EntityDBTask::presentMainThread();
}

//-------------------------------------------------------------------------------------
DBTaskAccountLogin::DBTaskAccountLogin(const Network::Address& addr, 
									   std::string& loginName, 
									   std::string& accountName, 
									   std::string& password, 
									   SERVER_ERROR_CODE retcode,
									   std::string& postdatas, 
									   std::string& getdatas,
									   bool needCheckPassword) :
DBTask(addr),
loginName_(loginName),
accountName_(accountName),
password_(password),
postdatas_(postdatas),
getdatas_(getdatas),
retcode_(retcode),
componentID_(0),
entityID_(0),
dbid_(0),
flags_(0),
deadline_(0),
needCheckPassword_(needCheckPassword)
{
}

//-------------------------------------------------------------------------------------
DBTaskAccountLogin::~DBTaskAccountLogin()
{
}

//-------------------------------------------------------------------------------------
bool DBTaskAccountLogin::db_thread_process()
{
	// 如果Interfaces已经判断不成功就没必要继续下去
	if(retcode_ != SERVER_SUCCESS)
	{
		ERROR_MSG(fmt::format("DBTaskAccountLogin::db_thread_process(): interfaces is failed!\n"));
		return false;
	}

	retcode_ = SERVER_ERR_OP_FAILED;

	if(accountName_.size() == 0)
	{
		ERROR_MSG(fmt::format("DBTaskAccountLogin::db_thread_process(): accountName is NULL!\n"));
		retcode_ = SERVER_ERR_NAME;
		return false;
	}

	ScriptDefModule* pModule = EntityDef::findScriptModule(DBUtil::accountScriptName());

	if(pModule == NULL)
	{
		ERROR_MSG(fmt::format("DBTaskAccountLogin::db_thread_process(): not found account script[{}], login[{}] failed!\n", 
			DBUtil::accountScriptName(), accountName_));

		retcode_ = SERVER_ERR_SRV_NO_READY;
		return false;
	}

	EntityTables& entityTables = EntityTables::findByInterfaceName(pdbi_->name());
	KBEEntityLogTable* pELTable = static_cast<KBEEntityLogTable*>
		(entityTables.findKBETable("kbe_entitylog"));

	KBE_ASSERT(pELTable);

	KBEAccountTable* pTable = static_cast<KBEAccountTable*>(entityTables.findKBETable("kbe_accountinfos"));
	KBE_ASSERT(pTable);

	ACCOUNT_INFOS info;
	info.dbid = 0;
	info.flags = 0;
	info.deadline = 0;
	if(!pTable->queryAccount(pdbi_, accountName_, info))
	{
		flags_ = info.flags;
		deadline_ = info.deadline;
		if(ACCOUNT_TYPE(g_kbeSrvConfig.getLoginApp().account_type) != ACCOUNT_TYPE_NORMAL)
		{
			if (email_isvalid(accountName_.c_str()))
			{
				ERROR_MSG(fmt::format("DBTaskAccountLogin::db_thread_process(): account[{}] is email, autocreate failed!\n", 
					accountName_));

				retcode_ = SERVER_ERR_CANNOT_USE_MAIL;
				return false;
			}
		}

		if (g_kbeSrvConfig.getDBMgr().notFoundAccountAutoCreate || 
			(Network::Address::NONE != g_kbeSrvConfig.interfacesAddr() && !needCheckPassword_/*第三方处理成功则自动创建账号*/))
		{
			if(!DBTaskCreateAccount::writeAccount(pdbi_, accountName_, password_, postdatas_, info) || info.dbid == 0 || info.flags != ACCOUNT_FLAG_NORMAL)
			{
				ERROR_MSG(fmt::format("DBTaskAccountLogin::db_thread_process(): writeAccount[{}] is error!\n",
					accountName_));

				retcode_ = SERVER_ERR_DB;
				return false;
			}

			INFO_MSG(fmt::format("DBTaskAccountLogin::db_thread_process(): not found account[{}], autocreate successfully!\n", 
				accountName_));

			info.password = KBE_MD5::getDigest(password_.data(), (int)password_.length());
		}
		else
		{
			ERROR_MSG(fmt::format("DBTaskAccountLogin::db_thread_process(): not found account[{}], login failed!\n", 
				accountName_));

			retcode_ = SERVER_ERR_NOT_FOUND_ACCOUNT;
			return false;
		}
	}

	if(info.dbid == 0 || info.flags != ACCOUNT_FLAG_NORMAL)
		return false;

	if (needCheckPassword_ || Network::Address::NONE == g_kbeSrvConfig.interfacesAddr())
	{
		if (kbe_stricmp(info.password.c_str(), KBE_MD5::getDigest(password_.data(), (int)password_.length()).c_str()) != 0)
		{
			retcode_ = SERVER_ERR_PASSWORD;
			return false;
		}
	}

	pTable->updateCount(pdbi_, accountName_, info.dbid);

	retcode_ = SERVER_ERR_ACCOUNT_IS_ONLINE;
	KBEEntityLogTable::EntityLog entitylog;
	bool success = !pELTable->queryEntity(pdbi_, info.dbid, entitylog, pModule->getUType());

	// 如果有在线纪录
	if(!success)
	{
		componentID_ = entitylog.componentID;
		entityID_ = entitylog.entityID;
	}
	else
	{
		retcode_ = SERVER_SUCCESS;
	}

	dbid_ = info.dbid;
	flags_ = info.flags;
	deadline_ = info.deadline;
	return false;
}

//-------------------------------------------------------------------------------------
thread::TPTask::TPTaskState DBTaskAccountLogin::presentMainThread()
{
	DEBUG_MSG(fmt::format("Dbmgr::onAccountLogin:loginName={0}, accountName={1}, success={2}, componentID={3}, dbid={4}, flags={5}, deadline={6}.\n", 
		loginName_,
		accountName_,
		retcode_,
		componentID_,
		dbid_,
		flags_,
		deadline_
		));

	// 一个用户登录， 构造一个数据库查询指令并加入到执行队列， 执行完毕将结果返回给loginapp
	Network::Bundle* pBundle = Network::Bundle::createPoolObject();
	(*pBundle).newMessage(LoginappInterface::onLoginAccountQueryResultFromDbmgr);

	(*pBundle) << retcode_;
	(*pBundle) << loginName_;
	(*pBundle) << accountName_;
	(*pBundle) << password_;
	(*pBundle) << componentID_;   // 如果大于0则表示账号还存活在某个baseapp上
	(*pBundle) << entityID_;
	(*pBundle) << dbid_;
	(*pBundle) << flags_;
	(*pBundle) << deadline_;
	(*pBundle).appendBlob(getdatas_);

	if(!this->send(pBundle))
	{
		ERROR_MSG(fmt::format("DBTaskAccountLogin::presentMainThread: channel({}) not found.\n", addr_.c_str()));
		Network::Bundle::reclaimPoolObject(pBundle);
	}

	return DBTask::presentMainThread();
}

//-------------------------------------------------------------------------------------
DBTaskQueryEntity::DBTaskQueryEntity(const Network::Address& addr, int8 queryMode, std::string& entityType, DBID dbid, 
		COMPONENT_ID componentID, CALLBACK_ID callbackID, ENTITY_ID entityID):
EntityDBTask(addr, entityID, dbid),
queryMode_(queryMode),
entityType_(entityType),
dbid_(dbid),
componentID_(componentID),
callbackID_(callbackID),
success_(false),
s_(),
entityID_(entityID),
wasActive_(false),
wasActiveCID_(0),
wasActiveEntityID_(0)
{
}

//-------------------------------------------------------------------------------------
DBTaskQueryEntity::~DBTaskQueryEntity()
{
}

//-------------------------------------------------------------------------------------
bool DBTaskQueryEntity::db_thread_process()
{
	EntityTables& entityTables = EntityTables::findByInterfaceName(pdbi_->name());
	ScriptDefModule* pModule = EntityDef::findScriptModule(entityType_.c_str());
	success_ = entityTables.queryEntity(pdbi_, dbid_, &s_, pModule);

	if(success_)
	{
		// 先写log， 如果写失败则可能这个entity已经在线
		KBEEntityLogTable* pELTable = static_cast<KBEEntityLogTable*>
			(entityTables.findKBETable("kbe_entitylog"));
		KBE_ASSERT(pELTable);

		try
		{
			success_ = pELTable->logEntity(pdbi_, addr_.ipAsString(), addr_.port, dbid_, 
				componentID_, entityID_, pModule->getUType());
		}
		catch (std::exception & e)
		{
			DBException& dbe = static_cast<DBException&>(e);
			if(dbe.isLostConnection())
			{
				static_cast<DBInterfaceMysql*>(pdbi_)->processException(e);
				return true;
			}
			else
				success_ = false;
		}

		if(!success_)
		{
			KBEEntityLogTable::EntityLog entitylog;

			try
			{
				pELTable->queryEntity(pdbi_, dbid_, entitylog, pModule->getUType());
			}
			catch (std::exception & e)
			{
				DBException& dbe = static_cast<DBException&>(e);
				if(dbe.isLostConnection())
				{
					static_cast<DBInterfaceMysql*>(pdbi_)->processException(e);
					return true;
				}
				else
					success_ = false;
			}

			wasActiveCID_ = entitylog.componentID;
			wasActiveEntityID_ = entitylog.entityID;
			wasActive_ = true;
		}
	}

	return false;
}

//-------------------------------------------------------------------------------------
thread::TPTask::TPTaskState DBTaskQueryEntity::presentMainThread()
{
	DEBUG_MSG(fmt::format("Dbmgr::DBTaskQueryEntity: {}, dbid={}, entityID={}, wasActive={}, queryMode={}, success={}.\n", 
		entityType_, dbid_, entityID_, wasActive_, ((int)queryMode_), success_));

	Network::Bundle* pBundle = Network::Bundle::createPoolObject();

	if(queryMode_ == 0)
		pBundle->newMessage(BaseappInterface::onCreateBaseFromDBIDCallback);
	else if(queryMode_ == 1)
		pBundle->newMessage(BaseappInterface::onCreateBaseAnywhereFromDBIDCallback);

	(*pBundle) << pdbi_->dbIndex();
	(*pBundle) << entityType_;
	(*pBundle) << dbid_;
	(*pBundle) << callbackID_;
	(*pBundle) << success_;
	(*pBundle) << entityID_;
	(*pBundle) << wasActive_;

	if(wasActive_)
	{
		(*pBundle) << wasActiveCID_;
		(*pBundle) << wasActiveEntityID_;
	}

	if(success_)
	{
		pBundle->append(s_);
	}

	if(!this->send(pBundle))
	{
		ERROR_MSG(fmt::format("DBTaskQueryEntity::presentMainThread: channel({}) not found.\n", addr_.c_str()));
		Network::Bundle::reclaimPoolObject(pBundle);
	}

	return EntityDBTask::presentMainThread();
}

//-------------------------------------------------------------------------------------
}
