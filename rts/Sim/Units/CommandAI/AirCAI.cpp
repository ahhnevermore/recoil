/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */


#include "AirCAI.h"
#include "Game/GameHelper.h"
#include "Game/GlobalUnsynced.h"
#include "Game/SelectedUnitsHandler.h"
#include "Map/Ground.h"
#include "Map/ReadMap.h"
#include "Sim/Misc/ModInfo.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/MoveTypes/StrafeAirMoveType.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitHandler.h"
#include "Sim/Weapons/Weapon.h"
#include "Sim/Weapons/WeaponDefHandler.h"
#include "System/myMath.h"
#include "System/Log/ILog.h"
#include "System/StringUtil.h"

#include <cassert>
#define AUTO_GENERATE_ATTACK_ORDERS 1

// AirCAI is always assigned to StrafeAirMoveType aircraft
static CStrafeAirMoveType* GetStrafeAirMoveType(const CUnit* owner) {
	assert(owner->unitDef->IsAirUnit());

	if (owner->UsingScriptMoveType())
		return static_cast<CStrafeAirMoveType*>(owner->prevMoveType);

	return static_cast<CStrafeAirMoveType*>(owner->moveType);
}



CR_BIND_DERIVED(CAirCAI, CMobileCAI, )
CR_REG_METADATA(CAirCAI, (
	CR_MEMBER(basePos),
	CR_MEMBER(baseDir),

	CR_MEMBER(activeCommand),
	CR_MEMBER(targetAge),

	CR_MEMBER(lastPC1),
	CR_MEMBER(lastPC2)
))

CAirCAI::CAirCAI()
	: CMobileCAI()
	, activeCommand(0)
	, targetAge(0)
	, lastPC1(-1)
	, lastPC2(-1)
{}

CAirCAI::CAirCAI(CUnit* owner)
	: CMobileCAI(owner)
	, activeCommand(0)
	, targetAge(0)
	, lastPC1(-1)
	, lastPC2(-1)
{
	cancelDistance = 16000;

	if (owner->unitDef->canAttack) {
		SCommandDescription c;

		c.id   = CMD_AREA_ATTACK;
		c.type = CMDTYPE_ICON_AREA;

		c.action    = "areaattack";
		c.name      = "Area attack";
		c.tooltip   = c.name + ": Sets the aircraft to attack enemy units within a circle";
		c.mouseicon = c.name;
		possibleCommands.push_back(commandDescriptionCache->GetPtr(c));
	}

	basePos = owner->pos;
}

void CAirCAI::GiveCommandReal(const Command& c, bool fromSynced)
{
	// take care not to allow aircraft to be ordered to move out of the map
	if ((c.GetID() != CMD_MOVE) && !AllowedCommand(c, true))
		return;

	if (c.GetID() == CMD_MOVE && c.GetNumParams() >= 3 &&
			(c.GetParam(0) < 0.0f || c.GetParam(2) < 0.0f
			 || c.GetParam(0) > (mapDims.mapx * SQUARE_SIZE)
			 || c.GetParam(2) > (mapDims.mapy * SQUARE_SIZE)))
	{
		return;
	}

	if (c.GetID() == CMD_SET_WANTED_MAX_SPEED)
		return;

	{
		CStrafeAirMoveType* airMT = GetStrafeAirMoveType(owner);

		if (c.GetID() == CMD_AUTOREPAIRLEVEL) {
			if (c.GetNumParams() == 0)
				return;

			switch ((int) c.GetParam(0)) {
				case 0: { repairBelowHealth = 0.0f; break; }
				case 1: { repairBelowHealth = 0.3f; break; }
				case 2: { repairBelowHealth = 0.5f; break; }
				case 3: { repairBelowHealth = 0.8f; break; }
				default: { /*no op*/ } break;
			}

			for (unsigned int n = 0; n < possibleCommands.size(); n++) {
				if (possibleCommands[n]->id != CMD_AUTOREPAIRLEVEL)
					continue;

				UpdateCommandDescription(n, c);
				break;
			}

			selectedUnitsHandler.PossibleCommandChange(owner);
			return;
		}

		if (c.GetID() == CMD_IDLEMODE) {
			if (c.GetNumParams() == 0)
				return;

			switch ((int) c.GetParam(0)) {
				case 0: { airMT->autoLand = false; break; }
				case 1: { airMT->autoLand = true;  break; }
				default: { /*no op*/ } break;
			}

			for (unsigned int n = 0; n < possibleCommands.size(); n++) {
				if (possibleCommands[n]->id != CMD_IDLEMODE)
					continue;

				UpdateCommandDescription(n, c);
				break;
			}

			selectedUnitsHandler.PossibleCommandChange(owner);
			return;
		}
	}

	if (!(c.GetOpts() & SHIFT_KEY) && nonQueingCommands.find(c.GetID()) == nonQueingCommands.end()) {
		activeCommand = 0;
		tempOrder = false;
	}

	if (c.GetID() == CMD_AREA_ATTACK && c.GetNumParams() < 4) {
		Command c2(CMD_ATTACK, c.GetOpts());
		c2.CopyParams(c);
		CCommandAI::GiveAllowedCommand(c2);
		return;
	}

	CCommandAI::GiveAllowedCommand(c);
}

void CAirCAI::SlowUpdate()
{
	// Commands issued may invoke SlowUpdate when paused
	if (gs->paused)
		return;

	if (!commandQue.empty() && (commandQue.front().GetTimeOut() < gs->frameNum)) {
		StopMoveAndFinishCommand();
		return;
	}

	// avoid the invalid (CStrafeAirMoveType*) cast
	if (owner->UsingScriptMoveType())
		return;


	#if (AUTO_GENERATE_ATTACK_ORDERS == 1)
	if (commandQue.empty()) {
		if (!AirAutoGenerateTarget(GetStrafeAirMoveType(owner))) {
			// if no target found, queue is still empty so bail now
			return;
		}
	}
	#endif

	// FIXME: check owner->UsingScriptMoveType() and skip rest if true?
	AAirMoveType* myPlane = GetStrafeAirMoveType(owner);
	Command& c = commandQue.front();

	if (c.GetID() == CMD_WAIT) {
		if ((myPlane->aircraftState == AAirMoveType::AIRCRAFT_FLYING)
		    	&& !owner->unitDef->DontLand() && myPlane->autoLand)
		{
			StopMove();
		}
		return;
	}

	switch (c.GetID()) {
		case CMD_AREA_ATTACK: {
			ExecuteAreaAttack(c);
			return;
		}
		default: {
			CMobileCAI::Execute();
			return;
		}
	}
}

bool CAirCAI::AirAutoGenerateTarget(AAirMoveType* myPlane) {
	assert(commandQue.empty());
	assert(myPlane->owner == owner);

	const UnitDef* ownerDef = owner->unitDef;
	const bool autoLand = !ownerDef->DontLand() && myPlane->autoLand;
	const bool autoAttack = ((owner->fireState >= FIRESTATE_FIREATWILL) && (owner->moveState != MOVESTATE_HOLDPOS));

	if (myPlane->aircraftState == AAirMoveType::AIRCRAFT_FLYING && autoLand) {
		StopMove();
	}

	if (ownerDef->canAttack && autoAttack && owner->maxRange > 0) {
		if (ownerDef->IsFighterAirUnit()) {
			const float3 P = owner->pos + (owner->speed * 10.0);
			const float R = 1000.0f * owner->moveState;
			const CUnit* enemy = CGameHelper::GetClosestEnemyAircraft(NULL, P, R, owner->allyteam);

			if (IsValidTarget(enemy)) {
				Command nc(CMD_ATTACK, INTERNAL_ORDER, enemy->id);
				commandQue.push_front(nc);
				inCommand = false;
				return true;
			}
		} else {
			const float3 P = owner->pos + (owner->speed * 20.0f);
			const float R = 500.0f * owner->moveState;
			const CUnit* enemy = CGameHelper::GetClosestValidTarget(P, R, owner->allyteam, this);

			if (enemy != NULL) {
				Command nc(CMD_ATTACK, INTERNAL_ORDER, enemy->id);
				commandQue.push_front(nc);
				inCommand = false;
				return true;
			}
		}
	}

	return false;
}


void CAirCAI::ExecuteMove(Command& c)
{
	float3 cmdPos = c.GetPos(0);

	AAirMoveType* myPlane = GetStrafeAirMoveType(owner);
	SetGoal(cmdPos, owner->pos);

	const CStrafeAirMoveType* airMT = (!owner->UsingScriptMoveType())? static_cast<const CStrafeAirMoveType*>(myPlane): NULL;
	const float radius = (airMT != NULL)? std::max(airMT->turnRadius + 2 * SQUARE_SIZE, 128.f) : 127.f;

	// we're either circling or will get to the target in 8 frames
	if ((owner->pos - cmdPos).SqLength2D() < (radius * radius)
			|| (owner->pos + owner->speed*8 - cmdPos).SqLength2D() < 127*127)
	{
		StopMoveAndFinishCommand();
	}
}


void CAirCAI::ExecuteFight(Command& c)
{
	assert((c.GetOpts() & INTERNAL_ORDER) || owner->unitDef->canFight);

	// FIXME: check owner->UsingScriptMoveType() and skip rest if true?
	AAirMoveType* myPlane = GetStrafeAirMoveType(owner);

	assert(owner == myPlane->owner);

	if (tempOrder) {
		tempOrder = false;
		inCommand = true;
	}

	if (c.GetNumParams() < 3) {
		LOG_L(L_ERROR, "[AirCAI::%s][f=%d][id=%d][#c.params=%d min=3]", __func__, gs->frameNum, owner->id, c.GetNumParams());
		return;
	}

	if (c.GetNumParams() >= 6) {
		if (!inCommand) {
			commandPos1 = c.GetPos(3);
		}
	} else {
		// HACK to make sure the line (commandPos1,commandPos2) is NOT
		// rotated (only shortened) if we reach this because the previous return
		// fight command finished by the 'if((curPos-pos).SqLength2D()<(127*127)){'
		// condition, but is actually updated correctly if you click somewhere
		// outside the area close to the line (for a new command).
		commandPos1 = ClosestPointOnLine(commandPos1, commandPos2, owner->pos);

		if ((owner->pos - commandPos1).SqLength2D() > (150 * 150)) {
			commandPos1 = owner->pos;
		}
	}

	float3 goalPos = c.GetPos(0);

	if (!inCommand) {
		inCommand = true;
		commandPos2 = goalPos;
	}
	if (c.GetNumParams() >= 6) {
		goalPos = ClosestPointOnLine(commandPos1, commandPos2, owner->pos);
	}

	// CMD_FIGHT is pretty useless if !canAttack, but we try to honour the modders wishes anyway...
	if (owner->unitDef->canAttack && (owner->fireState >= FIRESTATE_FIREATWILL)
			&& (owner->moveState != MOVESTATE_HOLDPOS) && (owner->maxRange > 0))
	{
		CUnit* enemy = NULL;

		if (owner->unitDef->IsFighterAirUnit()) {
			const float3 P = ClosestPointOnLine(commandPos1, commandPos2, owner->pos + owner->speed*10);
			const float R = 1000.0f * owner->moveState;

			enemy = CGameHelper::GetClosestEnemyAircraft(NULL, P, R, owner->allyteam);
		}
		if (IsValidTarget(enemy) && (owner->moveState != MOVESTATE_MANEUVER
				|| LinePointDist(commandPos1, commandPos2, enemy->pos) < 1000))
		{
			// make the attack-command inherit <c>'s options
			// (if <c> is internal, then so are the attacks)
			//
			// this is needed because CWeapon code will not
			// fire on "internal" targets if the weapon has
			// noAutoTarget set (although the <enemy> CUnit*
			// is technically not a user-target, we treat it
			// as such) even when explicitly told to fight
			Command nc(CMD_ATTACK, c.GetOpts(), enemy->id);
			commandQue.push_front(nc);

			tempOrder = true;
			inCommand = false;

			if (lastPC1 != gs->frameNum) { // avoid infinite loops
				lastPC1 = gs->frameNum;
				SlowUpdate();
			}
			return;
		} else {
			const float3 P = ClosestPointOnLine(commandPos1, commandPos2, owner->pos + owner->speed * 20);
			const float R = 500.0f * owner->moveState;

			enemy = CGameHelper::GetClosestValidTarget(P, R, owner->allyteam, this);

			if (enemy != nullptr) {
				PushOrUpdateReturnFight();

				// make the attack-command inherit <c>'s options
				Command nc(CMD_ATTACK, c.GetOpts(), enemy->id);
				commandQue.push_front(nc);

				tempOrder = true;
				inCommand = false;

				// avoid infinite loops (?)
				if (lastPC2 != gs->frameNum) {
					lastPC2 = gs->frameNum;
					SlowUpdate();
				}
				return;
			}
		}
	}

	ExecuteMove(c);
}

void CAirCAI::ExecuteAttack(Command& c)
{
	assert(owner->unitDef->canAttack);
	targetAge++;

	if (tempOrder && owner->moveState == MOVESTATE_MANEUVER) {
		// limit how far away we fly
		if (orderTarget && LinePointDist(commandPos1, commandPos2, orderTarget->pos) > 1500) {
			owner->DropCurrentAttackTarget();
			StopMoveAndFinishCommand();
			return;
		}
	}

	if (inCommand) {
		if (targetDied || (c.GetNumParams() == 1 && UpdateTargetLostTimer(int(c.GetParam(0))) == 0)) {
			StopMoveAndFinishCommand();
			return;
		}
		if (orderTarget != nullptr) {
			if (orderTarget->unitDef->canfly && orderTarget->IsCrashing()) {
				owner->DropCurrentAttackTarget();
				StopMoveAndFinishCommand();
				return;
			}
			if (!(c.GetOpts() & ALT_KEY) && SkipParalyzeTarget(orderTarget)) {
				owner->DropCurrentAttackTarget();
				StopMoveAndFinishCommand();
				return;
			}
		}
	} else {
		targetAge = 0;

		if (c.GetNumParams() == 1) {
			CUnit* targetUnit = unitHandler.GetUnit(c.GetParam(0));

			if (targetUnit == nullptr) {
				StopMoveAndFinishCommand();
				return;
			}
			if (targetUnit == owner) {
				StopMoveAndFinishCommand();
				return;
			}
			if (targetUnit->GetTransporter() != nullptr && !modInfo.targetableTransportedUnits) {
				StopMoveAndFinishCommand();
				return;
			}

			SetGoal(targetUnit->pos, owner->pos, cancelDistance);
			SetOrderTarget(targetUnit);
			owner->AttackUnit(targetUnit, (c.GetOpts() & INTERNAL_ORDER) == 0, false);

			inCommand = true;
		} else {
			SetGoal(c.GetPos(0), owner->pos, cancelDistance);
			owner->AttackGround(c.GetPos(0), (c.GetOpts() & INTERNAL_ORDER) == 0, false);

			inCommand = true;
		}
	}
}

void CAirCAI::ExecuteAreaAttack(Command& c)
{
	assert(owner->unitDef->canAttack);

	// FIXME: check owner->UsingScriptMoveType() and skip rest if true?
	AAirMoveType* myPlane = GetStrafeAirMoveType(owner);

	if (targetDied) {
		targetDied = false;
		inCommand = false;
	}

	const float3& pos = c.GetPos(0);
	const float radius = c.GetParam(3);

	if (inCommand) {
		if (myPlane->aircraftState == AAirMoveType::AIRCRAFT_LANDED)
			inCommand = false;

		if (orderTarget && orderTarget->pos.SqDistance2D(pos) > Square(radius)) {
			// target wandered out of the attack-area
			SetOrderTarget(NULL);
			SelectNewAreaAttackTargetOrPos(c);
		}
	} else {
		if (myPlane->aircraftState != AAirMoveType::AIRCRAFT_LANDED) {
			inCommand = true;

			SelectNewAreaAttackTargetOrPos(c);
		}
	}
}

void CAirCAI::ExecuteGuard(Command& c)
{
	assert(owner->unitDef->canGuard);

	const CUnit* guardee = unitHandler.GetUnit(c.GetParam(0));

	if (guardee == nullptr) {
		StopMoveAndFinishCommand();
		return;
	}
	if (UpdateTargetLostTimer(guardee->id) == 0) {
		StopMoveAndFinishCommand();
		return;
	}
	if (guardee->outOfMapTime > (GAME_SPEED * 5)) {
		StopMoveAndFinishCommand();
		return;
	}

	const bool pushAttackCommand =
		(owner->maxRange > 0.0f) &&
		owner->unitDef->canAttack &&
		((guardee->lastAttackFrame + 40) < gs->frameNum) &&
		IsValidTarget(guardee->lastAttacker);

	if (pushAttackCommand) {
		Command nc(CMD_ATTACK, c.GetOpts() | INTERNAL_ORDER, guardee->lastAttacker->id);
		commandQue.push_front(nc);
		SlowUpdate();
	} else {
		Command c2(CMD_MOVE, c.GetOpts() | INTERNAL_ORDER);
		c2.SetTimeOut(gs->frameNum + 60);

		if (guardee->pos.IsInBounds()) {
			c2.PushPos(guardee->pos);
		} else {
			float3 clampedGuardeePos = guardee->pos;

			clampedGuardeePos.ClampInBounds();

			c2.PushPos(clampedGuardeePos);
		}

		commandQue.push_front(c2);
	}
}

int CAirCAI::GetDefaultCmd(const CUnit* pointed, const CFeature* feature)
{
	if (pointed) {
		if (!teamHandler.Ally(gu->myAllyTeam, pointed->allyteam)) {
			if (owner->unitDef->canAttack) {
				return CMD_ATTACK;
			}
		} else {
			if (owner->unitDef->canGuard) {
				return CMD_GUARD;
			}
		}
	}
	return CMD_MOVE;
}

bool CAirCAI::IsValidTarget(const CUnit* enemy) const {
	if (!CMobileCAI::IsValidTarget(enemy)) return false;
	if (enemy->IsCrashing()) return false;
	return (GetStrafeAirMoveType(owner)->isFighter || !enemy->unitDef->canfly);
}



void CAirCAI::FinishCommand()
{
	targetAge = 0;
	CCommandAI::FinishCommand();
}

void CAirCAI::BuggerOff(const float3& pos, float radius)
{
	if (!owner->UsingScriptMoveType()) {
		static_cast<AAirMoveType*>(owner->moveType)->Takeoff();
	} else {
		CMobileCAI::BuggerOff(pos, radius);
	}
}


bool CAirCAI::SelectNewAreaAttackTargetOrPos(const Command& ac)
{
	assert(ac.GetID() == CMD_AREA_ATTACK);

	if (ac.GetID() != CMD_AREA_ATTACK)
		return false;

	const float3& pos = ac.GetPos(0);
	const float radius = ac.GetParam(3);

	std::vector<int> enemyUnitIDs;
	CGameHelper::GetEnemyUnits(pos, radius, owner->allyteam, enemyUnitIDs);

	if (enemyUnitIDs.empty()) {
		float3 attackPos = pos + (gsRNG.NextVector() * radius);
		attackPos.y = CGround::GetHeightAboveWater(attackPos.x, attackPos.z);

		owner->AttackGround(attackPos, (ac.GetOpts() & INTERNAL_ORDER) == 0, false);
		SetGoal(attackPos, owner->pos);
	} else {
		// note: the range of randFloat() is inclusive of 1.0f
		const unsigned int unitIdx = std::min<int>(gsRNG.NextFloat() * enemyUnitIDs.size(), enemyUnitIDs.size() - 1);
		const unsigned int unitID = enemyUnitIDs[unitIdx];

		CUnit* targetUnit = unitHandler.GetUnitUnsafe(unitID);

		SetOrderTarget(targetUnit);
		owner->AttackUnit(targetUnit, (ac.GetOpts() & INTERNAL_ORDER) == 0, false);
		SetGoal(targetUnit->pos, owner->pos);
	}

	return true;
}

