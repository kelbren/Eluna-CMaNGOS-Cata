/*
 * This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Entities/Totem.h"
#include "Log/Log.h"
#include "Groups/Group.h"
#include "Entities/Player.h"
#include "Globals/ObjectMgr.h"
#include "Spells/SpellMgr.h"
#include "AI/BaseAI/CreatureAI.h"
#include "Maps/InstanceData.h"
#ifdef BUILD_ELUNA
#include "LuaEngine/LuaEngine.h"
#endif

Totem::Totem() : Creature(CREATURE_SUBTYPE_TOTEM)
{
    m_duration = 0;
    m_type = TOTEM_PASSIVE;
}

bool Totem::Create(uint32 guidlow, CreatureCreatePos& cPos, CreatureInfo const* cinfo, Unit* owner)
{
    SetMap(cPos.GetMap());
    SetPhaseMask(cPos.GetPhaseMask(), false);

    Team team = owner->GetTypeId() == TYPEID_PLAYER ? ((Player*)owner)->GetTeam() : TEAM_NONE;

    if (!CreateFromProto(guidlow, cinfo, team))
        return false;

    // special model selection case for totems
    if (owner->GetTypeId() == TYPEID_PLAYER)
    {
        if (uint32 modelid_race = sObjectMgr.GetModelForRace(GetNativeDisplayId(), owner->getRaceMask()))
            SetDisplayId(modelid_race);
    }

    cPos.SelectFinalPoint(this);

    // totem must be at same Z in case swimming caster and etc.
    if (fabs(cPos.m_pos.z - owner->GetPositionZ()) > 5.0f)
        cPos.m_pos.z = owner->GetPositionZ();

    if (!cPos.Relocate(this))
        return false;

    // Notify the map's instance data.
    // Only works if you create the object in it, not if it is moves to that map.
    // Normally non-players do not teleport to other maps.
    if (InstanceData* iData = GetMap()->GetInstanceData())
        iData->OnCreatureCreate(this);

    LoadCreatureAddon(false);

    SetCanDodge(false);
    SetCanParry(false);
    SetCanBlock(false);

    return true;
}

void Totem::Update(uint32 update_diff, uint32 time)
{
    Unit* owner = GetOwner();
    if (!owner || !owner->IsAlive() || !IsAlive())
    {
        UnSummon();                                         // remove self
        return;
    }

    if (m_duration <= update_diff)
    {
        UnSummon();                                         // remove self
        return;
    }
    else
        m_duration -= update_diff;

    Creature::Update(update_diff, time);
}

void Totem::Summon(Unit* owner)
{
    owner->GetMap()->Add((Creature*)this);
    AIM_Initialize();

    if (owner->AI())
        owner->AI()->JustSummoned((Creature*)this);

#ifdef BUILD_ELUNA
    if (Eluna* e = GetEluna())
        e->OnSummoned(this, owner);
#endif

    // there are some totems, which exist just for their visual appeareance
    if (!GetSpell())
        return;

    switch (m_type)
    {
        case TOTEM_PASSIVE:
            CastSpell(this, GetSpell(), TRIGGERED_OLD_TRIGGERED);
            break;
        case TOTEM_STATUE:
            CastSpell(GetOwner(), GetSpell(), TRIGGERED_OLD_TRIGGERED);
            break;
        default: break;
    }
}

void Totem::UnSummon()
{
    CombatStop();
    RemoveAurasDueToSpell(GetSpell());

    if (Unit* owner = GetOwner())
    {
        owner->_RemoveTotem(this);
        owner->RemoveAurasDueToSpell(GetSpell());

        // remove aura all party members too
        if (owner->GetTypeId() == TYPEID_PLAYER)
        {
            ((Player*)owner)->SendAutoRepeatCancel(this);

            // Not only the player can summon the totem (scripted AI)
            if (Group* pGroup = ((Player*)owner)->GetGroup())
            {
                for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
                {
                    Player* Target = itr->getSource();
                    if (Target && pGroup->SameSubGroup((Player*)owner, Target))
                        Target->RemoveAurasDueToSpell(GetSpell());
                }
            }
        }

        if (owner->AI())
            owner->AI()->SummonedCreatureDespawn((Creature*)this);
    }

    // any totem unsummon look like as totem kill, req. for proper animation
    if (IsAlive())
        SetDeathState(DEAD);

    AddObjectToRemoveList();
}

void Totem::SetTypeBySummonSpell(SpellEntry const* spellProto)
{
    // Get spell casted by totem
    SpellEntry const* totemSpell = sSpellTemplate.LookupEntry<SpellEntry>(GetSpell());
    if (totemSpell)
    {
        // If spell have cast time -> so its active totem
        if (GetSpellCastTime(totemSpell))
            m_type = TOTEM_ACTIVE;
    }
    if (spellProto->SpellIconID == 2056)
        m_type = TOTEM_STATUE;                              // Jewelery statue
}

float Totem::GetCritChance(WeaponAttackType attackType) const
{
    // Totems use owner's crit chance (when owner is available)
    if (const Unit* owner = GetOwner())
        return owner->GetCritChance(attackType);
    return Creature::GetCritChance(attackType);
}

float Totem::GetCritChance(SpellSchoolMask schoolMask) const
{
    // Totems use owner's crit chance (when owner is available)
    if (const Unit* owner = GetOwner())
        return owner->GetCritChance(schoolMask);
    return Creature::GetCritChance(schoolMask);
}

float Totem::GetCritMultiplier(SpellSchoolMask dmgSchoolMask, uint32 creatureTypeMask, const SpellEntry *spell, bool heal) const
{
    // Totems use owner's crit multiplier
    if (const Unit* owner = GetOwner())
        return owner->GetCritMultiplier(dmgSchoolMask, creatureTypeMask, spell, heal);
    return Creature::GetCritMultiplier(dmgSchoolMask, creatureTypeMask, spell, heal);
}

float Totem::GetHitChance(WeaponAttackType attackType) const
{
    // Totems use owner's hit chance (when owner is available)
    if (const Unit* owner = GetOwner())
        return owner->GetHitChance(attackType);
    return Creature::GetHitChance(attackType);
}

float Totem::GetHitChance(SpellSchoolMask schoolMask) const
{
    // Totems use owner's hit chance (when owner is available)
    if (const Unit* owner = GetOwner())
        return owner->GetHitChance(schoolMask);
    return Creature::GetHitChance(schoolMask);
}

float Totem::GetMissChance(WeaponAttackType /*attackType*/) const
{
    // Totems have no inherit miss chance
    return 0.0f;
}

float Totem::GetMissChance(SpellSchoolMask /*schoolMask*/) const
{
    // Totems have no inherit miss chance
    return 0.0f;
}

int32 Totem::GetResistancePenetration(SpellSchools school) const
{
    // Totems use owner's penetration (when owner is available)
    if (const Unit* owner = GetOwner())
        return owner->GetResistancePenetration(school);
    return Creature::GetResistancePenetration(school);
}

bool Totem::IsImmuneToSpellEffect(SpellEntry const* spellInfo, SpellEffectIndex index, bool castOnSelf) const
{
    // Totem may affected by some specific spells
    // Mana Spring, Healing stream, Mana tide
    // Flags : 0x00000002000 | 0x00000004000 | 0x00004000000 -> 0x00004006000
    if (spellInfo->SpellFamilyName == SPELLFAMILY_SHAMAN && spellInfo->IsFitToFamilyMask(uint64(0x00004006000)))
        return false;

        // TODO: possibly all negative auras immune?
        switch(spellInfo->Effect[index])
        {
            case SPELL_EFFECT_ATTACK_ME:
                return true;
            default:
                break;
        }
        switch(spellInfo->EffectApplyAuraName[index])
        {
            case SPELL_AURA_PERIODIC_DAMAGE:
            case SPELL_AURA_PERIODIC_LEECH:
            case SPELL_AURA_MOD_FEAR:
            case SPELL_AURA_TRANSFORM:
            case SPELL_AURA_MOD_TAUNT:
                return true;
            default:
                break;
        }

    return Creature::IsImmuneToSpellEffect(spellInfo, index, castOnSelf);
}
