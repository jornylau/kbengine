# -*- coding: utf-8 -*-
import KBEngine
import wtimer
import time
import random
import GlobalDefine
from KBEDebug import * 

class AI:
	def __init__(self):
		pass
	
	def addTerritory(self):
		"""
		添加领地
		进入领地范围的某些entity将视为敌人
		"""
		xzrange = 30.0
		INFO_MSG("%s::addTerritory: %i range=%i." % (self.getScriptName(), self.id, xzrange))
		assert self.territoryControllerID == 0 and "territoryControllerID != 0"
		self.territoryControllerID = self.addProximity(xzrange, 0, 0)
		
		if self.territoryControllerID <= 0:
			ERROR_MSG("%s::addTerritory: %i, range=%i, is error!" % (self.getScriptName(), self.id, xzrange))
	
	def delTerritory(self):
		"""
		删除领地
		"""
		if self.territoryControllerID > 0:
			self.cancel(self.territoryControllerID)
			self.territoryControllerID = 0
			INFO_MSG("%s::delTerritory: %i" % (self.getScriptName(), self.id))
			
	def enable(self):
		"""
		激活entity
		"""
		self.heartBeatTimerID = \
		self.addTimer(random.randint(0, 1), 1, wtimer.TIMER_TYPE_HEARDBEAT)				# 心跳timer, 每1秒一次
		
	def disable(self):
		"""
		禁止这个entity做任何行为
		"""
		self.delTimer(self.heartBeatTimerID)
		self.heartBeatTimerID = 0
			
	def onHeardTimer(self, tid, tno):
		"""
		entity的心跳
		"""
		self.think()
	
	def think(self):
		"""
		virtual method.
		"""
		if self.isState(GlobalDefine.ENTITY_STATE_FREE):
			self.onThinkFree()
		elif self.isState(GlobalDefine.ENTITY_STATE_FIGHT):
			self.onThinkFight()
		else:
			self.onThinkOther()
			
	# ----------------------------------------------------------------
	# callback
	# ----------------------------------------------------------------
	def onWitnessed(self, isWitnessed):
		"""
		KBEngine method.
		此实体是否被观察者(player)观察到, 此接口主要是提供给服务器做一些性能方面的优化工作，
		在通常情况下，一些entity不被任何客户端所观察到的时候， 他们不需要做任何工作， 利用此接口
		可以在适当的时候激活或者停止这个entity的任意行为。
		@param isWitnessed	: 为false时， entity脱离了任何观察者的观察
		"""
		INFO_MSG("%s::onWitnessed: %i isWitnessed=%i." % (self.getScriptName(), self.id, isWitnessed))
		
		if isWitnessed:
			self.enable()
		else:
			self.disable()
			
	def onThinkFree(self):
		"""
		virtual method.
		闲置时think
		"""
		if self.territoryControllerID <= 0:
			self.addTerritory()
		
		self.randomWalk(self.spawnPos)

	def onThinkFight(self):
		"""
		virtual method.
		战斗时think
		"""
		pass

	def onThinkOther(self):
		"""
		virtual method.
		其他时think
		"""
		pass
		
	def onForbidChanged_(self, forbid, isInc):
		"""
		virtual method.
		entity禁止 条件改变
		@param isInc		:	是否是增加
		"""
		pass

	def onStateChanged_(self, oldstate, newstate):
		"""
		virtual method.
		entity状态改变了
		"""
		pass

	def onSubStateChanged_(self, oldSubState, newSubState):
		"""
		virtual method.
		子状态改变了
		"""
		#INFO_MSG("%i oldSubstate=%i to newSubstate=%i" % (self.id, oldSubState, newSubState))
		pass

	def onFlagsChanged_(self, flags, isInc):
		"""
		virtual method.
		"""
		pass
	
	def onEnterTrap(self, entityEntering, range_xz, range_y, controllerID, userarg):
		"""
		KBEngine method.
		有entity进入trap
		"""
		if controllerID != self.heartBeatTimerID:
			return
		
		DEBUG_MSG("%s::onEnterTrap: %i entityEntering=(%s)%i, range_xz=%s, range_y=%s, controllerID=%i, userarg=%i" % \
						(self.getScriptName(), self.id, entityEntering.getScriptName(), entityEntering.id, \
						range_xz, range_y, controllerID, userarg))

	def onLeaveTrap(self, entityLeaving, range_xz, range_y, controllerID, userarg):
		"""
		KBEngine method.
		有entity离开trap
		"""
		if controllerID != self.heartBeatTimerID:
			return
		
		INFO_MSG("%s::onLeaveTrap: %i entityLeaving=(%s)%i." % (self.getScriptName(), self.id, \
				entityLeaving.getScriptName(), entityLeaving.id))
				
AI._timermap = {}
AI._timermap[wtimer.TIMER_TYPE_HEARDBEAT] = AI.onHeardTimer