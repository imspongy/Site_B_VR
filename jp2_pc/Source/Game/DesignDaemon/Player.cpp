/**********************************************************************************************
 *
 * Copyright © DreamWorks Interactive. 1996
 *
 * Contents: The implementation of Player.hpp.
 *
 * Bugs:
 *
 * To do:
 *
 **********************************************************************************************
 *
 * $Log:: /JP2_PC/Source/Game/DesignDaemon/Player.cpp                                         $
 * 
 * 229   10/08/98 5:39p Mlange
 * Reduced camera shake parameters.
 * 
 * 228   98/10/08 8:16 Speter
 * Removed recent code which ignored intersecting objects on pickup. Wasn't necessary once
 * interpenetration bug was fixed.
 * 
 * 227   10/08/98 3:08a Agrant
 * upped swing damage a tad
 * 
 * 226   98/10/08 0:17 Speter
 * Now upon pickup, any intersected objects are ignored. Fixed stow drift, again. Tweaked back
 * stow path to move out to side more.
 * 
 * 225   98/10/07 6:54 Speter
 * Fixed stow crash; now hand ignores foot during stow and throw. Fixed stow drift by storing
 * relative placements. Moved back stow back a bit, so you can't see rocks and things.
 * 
 *********************************************************************************************/

//
// Includes.
//

#include "gblinc/common.hpp"
#include "Player.hpp"

#include "PlayerSettings.hpp"

#include "Game/AI/AIInfo.hpp"
#include "Lib/GeomDBase/PartitionPriv.hpp"
#include "Lib/GeomDBase/RayCast.hpp"
#include "Lib/GeomDBase/Skeleton.hpp"
#include "Lib/EntityDBase/Entity.hpp"
#include "Lib/EntityDBase/Animal.hpp"
#include "Lib/EntityDBase/WorldDBase.hpp"
#include "Lib/EntityDBase/Message.hpp"
#include "Lib/EntityDBase/MessageTypes/MsgTrigger.hpp"
#include "Lib/EntityDBase/MessageTypes/MsgStep.hpp"
#include "Lib/EntityDBase/MessageTypes/MsgMove.hpp"
#include "Lib/EntityDBase/MessageTypes/MsgCollision.hpp"
#include "Lib/EntityDBase/MessageTypes/MsgControl.hpp"
#include "Lib/EntityDBase/MessageTypes/MsgPhysicsReq.hpp"
#include "Lib/EntityDBase/MessageTypes/MsgSystem.hpp"
#include "Lib/EntityDBase/Teleport.hpp"
#include "Lib/EntityDBase/WorldDBase.hpp"
#include "Lib/EntityDBase/Query/QRenderer.hpp"
#include "Lib/EntityDBase/Query/QPhysics.hpp"
#include "Lib/Loader/SaveFile.hpp"
#include "Lib/Control/Control.hpp"
#include "Lib/Physics/InfoPlayer.hpp"
#include "Lib/Physics/Magnet.hpp"
#include "Lib/Physics/pelvis.h"
#include "Lib/Physics/PhysicsSystem.hpp"
#include "Lib/Physics/PhysicsHelp.hpp"
#include "Lib/Renderer/Camera.hpp"
#include "Lib/Audio/AudioDaemon.hpp"
#include "Lib/Transform/TransformIO.hpp"
#include "Lib/Sys/W95/Render.hpp"
#include "Lib/Sys/ConIO.hpp"
#include "Lib/Renderer/Overlay.hpp"
#include "Lib/Math/MathUtil.hpp"

#include "Lib/View/LineDraw.hpp"
#include "Lib/Sys/DebugConsole.hpp"
#include "Lib/Sys/Profile.hpp"
#include "Lib/EntityDBase/AnimationScript.hpp"
#include "Lib/Control/VRShared.hpp"
#include <vector>
#include "Lib/Physics/InfoSkeleton.hpp"
#include "Game/DesignDaemon/Gun.hpp"

//
// VR mod helpers.
//
namespace
{
	inline bool bVRHead()
	{
		return gVR.bActive && gVR.bHeadValid;
	}

	inline CRotate3<> r3VR(const float* af_q)
	{
		return CRotate3<>(af_q[0], af_q[1], af_q[2], af_q[3], true);
	}

}

// PREVENT CODE GEN BUG!
#pragma optimize("g", off)

#ifdef __MWERKS__
 // for != if only given ==
 #include <utility>
 using namespace std::rel_ops;
#endif


//
// Defines.
//

#define bDROP_THROW			0		// Position and throw object while dropping.
#define bCAMERA_DEATH		0		// Kooky film school death effect.
#define bHINGE_ADJUST		1		// Adjust hinge pickup to compensate for physics behaviour.


//
// Globals.
//
CPlayer *gpPlayer = 0;

extern CProfileStat psMoveMsgPlayer;
extern CProfileStat psCollisionMsgPlayer;

//**********************************************************************************************
//
// SPlayerSettings implementation.
//

	//******************************************************************************************
	SPlayerSettings::SPlayerSettings() :
		v3WristToPalmNormal	(0, .09, -.03),
		v3WristToPalmSpecial(0, .09, -.01),
		fHandDistMin		(.80),
		fHandDistMax		(.95),
		fHandReachSpecial	(1.25),
		fHandReachGrab		(1.5),
		fHandReachWidth		(.20),
		fHandDistPickup		(.20),

		sPickupTime			(0.5),
		sSwingTime			(1.0),
		sPickupNoDrop		(1.0),
		sRaiseTime			(0.5),
		sThrowTime			(0.3),

		fMaxMassPickup		(100.0),
		fMaxMassSwing		(5.0),
		fSwingDamageMul		(2.75),

		bHandFollowHead		(false),
		bMoveHandActual		(true),
		bActivateCentreEmpty(true),
		bActivateCentreHolding(false),
		bAllowDrop			(true),
		bAutoCrouchNormal	(false),
		bAutoCrouchSpecial	(true),
		sCrouchDelay		(0.40),
		fCrouchDist			(0.72),
		fAutoCrouchDist		(0.82),					// Require additional belowness.
		bHoldRelativeShoulder	(false),

		ang2HandMax			(65 * dDEGREES, 75 * dDEGREES),
		ang2HandTurnMax		(35 * dDEGREES, 60 * dDEGREES),
		ang2HandViewMin		(-5 * dDEGREES, -20 * dDEGREES),
		ang2HandViewMax		(30 * dDEGREES,  20 * dDEGREES),
		ang3HandRotMin		(-40 * dDEGREES, -85 * dDEGREES, - 95 * dDEGREES),
		ang3HandRotMax		(+40 * dDEGREES, +95 * dDEGREES, +175 * dDEGREES),
		ang3HandDropMin		(-120 * dDEGREES, -120 * dDEGREES, - 95 * dDEGREES),
		ang3HandDropMax		(+120 * dDEGREES, +120 * dDEGREES, +185 * dDEGREES),
		angHandRotPickup	(6 * dDEGREES),
		rHandRotPickup		(0.4),

		angThrowLob			(45 * dDEGREES),
		fThrowMomentumScale	(30.0),
		fThrowVelocityMax	(10.0),
		fThrowTorque		(0.3),					// Reduce torque a lot.
		bThrowReverse		(false),

		p3SafeHand			(CVector3<>(0, 0, 0)),
		angSwingPull		(35 * dDEGREES),

		v3HeadOffset		(0, 0.13, .60),
		v3NeckOffset		(0, 0.10, .56),
		bHeadFollowActual	(true),
		fHeadMoveFrac		(0.75),
		ang2HeadViewMax		(90 * dDEGREES, 69 * dDEGREES),
		fAngHeadTurnX		(30 * dDEGREES)
	{
		// Side-stow placements.
		aap3Stow[0][0]	= CPlacement3<>(CRotate3<>(d3ZAxis, dPI_2) * CRotate3<>(d3XAxis, -dPI_2 * 0.5),
										CVector3<>(-.10, .40, .35));
		aap3Stow[0][1]	= CPlacement3<>(CRotate3<>(d3ZAxis, dPI_2) * CRotate3<>(d3YAxis, -dPI_2),
										CVector3<>(-.20, .25, .25));
		aap3Stow[0][2]	= CPlacement3<>(CRotate3<>(d3ZAxis, dPI_2) * CRotate3<>(d3YAxis, -dPI_2),
										CVector3<>(-.20, .25, .05));

		// Back-stow placements.
		aap3Stow[1][0]	= CPlacement3<>(CRotate3<>(d3YAxis, dPI_2) * CRotate3<>(d3XAxis, dPI_2),
										CVector3<>(.20, .40, .80));
		aap3Stow[1][1]	= CPlacement3<>(CRotate3<>(d3YAxis, dPI_2 * 0.5) * CRotate3<>(d3XAxis, dPI),
										CVector3<>(.25, .0, .90));
		aap3Stow[1][2]	= CPlacement3<>(CRotate3<>(d3ZAxis, dPI   ) * CRotate3<>(d3XAxis, dPI_2),
										CVector3<>(.20, -.25, .60));

		// Side-stow limits.
		av3StowMin[0]	= CVector3<>(-.50, -.25, -.20);
		av3StowMax[0]	= CVector3<>( .25,  .80,  .25);

		// Back-stow limits.
		av3StowMin[1]	= CVector3<>(-1.00, -.75, -.35),
		av3StowMax[1]	= CVector3<>(  .50, 1.50,  .50),

		// Throw cock position.
		p3Throw			= CPlacement3<>(CRotate3<>(d3XAxis, dPI_2),
										CVector3<>(.30, .25, .60));
	}

	SPlayerSettings		PlayerSettings;

//**********************************************************************************************
//
namespace
//
// Private static implementation.
//
//**************************************
{
	typedef CMSArray<TSoundMaterial, 10> TSampleSet;


	static bool bAllowStab		= false;			// Don't kill yourself.

	//
	// Constants.
	//

	// Camera shake parameters.
	const float	fShakeMinSize	= 5.0f,				// Min animal radius for producing shake.
				fShakeMaxSize	= 10.0f,			// Max animal radius for producing shake.
				angShakeMax		= 1 * dDEGREES,		// Max amplitude of camera shake.
				fShakeDecay		= 2.0f,				// Exponential decay rate.
				sShakePeriod	= 0.15f,			// Frequency of shake cycle.
				fShakeJostle	= .6f;				// Max jostling applied to player.
													// (1 = jump velocity).

	//
	// Variables.
	//

	//
	// Private static functions.
	//

	//*****************************************************************************************
	//	
	CAngles2 Angles2
	(
		const CDir3<>& d3
	)
	//
	// Returns:
	//		A pair of angles representing the polar coordinates of the direction, with
	//		the Y axis at (0, 0).
	//
	//**********************************
	{
		return CAngles2
		(
			atan2(d3.tX, d3.tY),		// Theta (Yaw).
			asin(d3.tZ)					// Phi (Pitch).
		);
	}

	//*****************************************************************************************
	//	
	CAngles2 Angles2
	(
		const CRotate3<>& r3
	)
	//
	// Returns:
	//		A pair of angles representing the polar coordinates of the rotation, with
	//		the Y axis at (0, 0).
	//
	//**********************************
	{
		return Angles2(d3YAxis * r3);
	}

	//*****************************************************************************************
	// VR: headset orientation relative to the given body rotation, as yaw/pitch (no roll).
	inline CAngles2 ang2VRHead(const CRotate3<>& r3_body)
	{
		return Angles2(r3VR(gVR.afHeadRotWorld) / r3_body);
	}

	//*****************************************************************************************
	//	
	CAngles3 Angles3
	(
		const CRotate3<>& r3
	)
	//
	// Returns:
	//		Three angles representing the polar coordinates of the rotation:
	//		X = yaw, Y = pitch, Z = roll
	//
	//**********************************
	{
		CMatrix3<> mx3 = r3;

		CAngles3 ang3;

		// Pitch.
		ang3.tY = asin(-mx3.v3Z.tY);
		if (mx3.v3Y.tY < 0.0f)
		{
			if (ang3.tY > 0.0f)
				ang3.tY = dPI - ang3.tY;
			else
				ang3.tY = -dPI - ang3.tY;
		}

		// Roll.
		ang3.tZ = atan2(mx3.v3Z.tX, fSqrt(Sqr(mx3.v3Z.tZ) + Sqr(mx3.v3Z.tY)));

		// Since Z rotation can range from <-90 to >+180 deg, we change the splitting angle
		// to be halfway between, not at 180.
		if (ang3.tZ < -135 * dDEGREES)
			ang3.tZ += 360 * dDEGREES;

		// Yaw.
		ang3.tX = -atan2(mx3.v3X.tY, fSqrt(Sqr(mx3.v3X.tX) + Sqr(mx3.v3X.tZ)));

		return ang3;
	}

	//*****************************************************************************************
	//	
	CDir3<> d3FromAngles
	(
		const CAngles2& ang2
	)
	//
	// Returns:
	//		Direction of vector specified by angles.
	//
	//**********************************
	{
		// Treat angles as polar coords, return direction.
		float f_cos_y = cos(ang2.tY);
		return CDir3<>
		(
			sin(ang2.tX) * f_cos_y,
			cos(ang2.tX) * f_cos_y,
			sin(ang2.tY),
			true						// Already normalised.
		);
	}

	//*****************************************************************************************
	//	
	CRotate3<> r3FromAngles
	(
		const CAngles2& ang2
	)
	//
	// Y value rotates about X (pitch), then X value rotates about Z (yaw).
	//
	//**********************************
	{
		return CRotate3<>(d3XAxis, CAngle(ang2.tY)) *
			   CRotate3<>(d3ZAxis, CAngle(-ang2.tX));
	}

	//*****************************************************************************************
	//	
	CRotate3<> r3FromAngles
	(
		const CAngles3& ang3
	)
	//
	// X value rotates about -Z (yaw), Y value rotates about X (pitch), Z value rotates about Y (roll).
	//
	//**********************************
	{
		return CRotate3<>(d3ZAxis, CAngle(-ang3.tX)) *
			   CRotate3<>(d3XAxis, CAngle(ang3.tY)) *
			   CRotate3<>(d3YAxis, CAngle(ang3.tZ));
	}

	//*****************************************************************************************
	//	
	CRotate3<> r3FromDir
	(
		const CDir3<>& d3
	)
	//
	// First rotate about model Z, then local X.
	// To implement this, we construct rotations in reverse order.
	//
	//**********************************
	{
		// Find XY projection of direction (use non-zero Z component to avoid singularity).
		CDir3<> d3_xy(d3.tX, d3.tY, 0.0001);

		// Rotate first to XY projection (about Z), then down to final direction.
		return CRotate3<>(d3YAxis, d3_xy) * CRotate3<>(d3_xy, d3);
	}

	//*****************************************************************************************
	//
	CRotate3<> r3OrientSpace
	(
		const CDir3<>& d3_ref			// The reference direction defining the rotation space.
	)
	//
	// Returns:
	//		The space's Y axis is the vector from the shoulder to the target wrist,
	//		and its X axis must have no Z component.
	//
	//**********************************
	{
		// Rotate Y axis to the reference direction.
		CRotate3<> r3(d3YAxis, d3_ref);

		// Right it.
		CDir3<> d3_x = d3XAxis * r3;

		r3 *= CRotate3<>(d3_x, CDir3<>(d3_x.tX, d3_x.tY, 0));
		return r3;
	}


	//*****************************************************************************************
	inline CConsoleBuffer& operator <<(CConsoleBuffer& con, const CVector2<>& v2)
	{
		return con <<v2.tX <<',' <<v2.tY;
	}

	//*****************************************************************************************
	inline CConsoleBuffer& operator <<(CConsoleBuffer& con, const CVector3<>& v3)
	{
		return con <<v3.tX <<',' <<v3.tY <<',' <<v3.tZ;
	}

	//*****************************************************************************************
	void AdjustPickup(CPlacement3<>* pp3_pickup, const CDir3<>& d3_axis_obj, 
					  const CMagnetPair* pmp_pickup, const CPlacement3<>& p3_hand_cur)
	{
		//
		// Adjust pickup by rotation about object's swivel axis.
		// We wish to minimise the rotation necessary for the hand.
		//

		// Calculate current rotation necessary.
		CRotate3<> r3_move = ~p3_hand_cur.r3Rot * pp3_pickup->r3Rot;

		// Calculate the object's swivel axis in world space.
		CPlacement3<> p3_base = pmp_pickup->pinsMaster->pphiGetPhysicsInfo()->p3Base(pmp_pickup->pinsMaster);
		CDir3<> d3_axis = d3_axis_obj * p3_base.r3Rot;
		
		//
		// We can apply a rotation about d3_axis (A) of any angle to r3_move.
		// If we parameterise the rotation by the C element of the quaternion (t), then the
		// rotation is:
		//
		//		R = (t, sqrt(1 - t²)A)
		//
		// We wish to minimise the product of r3_move (Q) and R.  This is done by maximising
		// the absolute value of the product's C element.  By quaternion multiplication, this is:
		//
		//		C = Q.c R.c - Q.S R.S
		//		  = Q.c t - Q.S A sqrt(1-t²)
		//		  = X t - Y sqrt(1-t²)				(X == Q.c,  Y == Q.S A)
		//
		//		dC/dt = 0 = X - Y (-2t) / (2 sqrt(1-t²))
		//				  = X + Y t / sqrt(1-t²)
		//		Y t/sqrt(1-t²) = -X
		//		Y² t²/(1-t²) = X²
		//		t²/(1-t²) = X²/Y²
		//		t² = X²/Y² (1-t²)
		//		(1 + X²/Y²)t² = X²/Y²
		//		t² = X²/Y² / (1 + X²/Y²)
		//		t² = X²/(X² + Y²)
		//		t = ± X / sqrt(X² + Y²)
		//
		//	To find which sign of t yields the true maximum absolute value of C, examine C again:
		//
		//		C = X t - Y sqrt(1-t²)
		//
		//	X t should be the same sign as -Y, thus we want X Y t < 0.
		//		

		TReal r_dot = r3_move.v3S * d3_axis;
		TReal r_denom = Square(r3_move.tC) + Square(r_dot);
		if (r_denom)
		{
			TReal r_t	= r3_move.tC * fInvSqrt(r_denom);

			// Choose correct sign.
			if (r3_move.tC * r_dot * r_t > 0)
				// They are different signs; switch them.
				r_t = -r_t;

			CRotate3<> r3_adjust(r_t, d3_axis * fSqrt(1.0 - r_t * r_t));

			pp3_pickup->r3Rot *= r3_adjust;

#if VER_DEBUG
			CRotate3<> r3_move_new = ~p3_hand_cur.r3Rot * pp3_pickup->r3Rot;
			Assert(Abs(r3_move_new.tC) > Abs(r3_move.tC));
#endif
		}
	}

	//*****************************************************************************************
	CPlacement3<> p3SpecialPickup(const CMagnetPair* pmp_pickup, const CPlacement3<>& p3_palm)
	{
		// Return the world placement of the special pickup magnet.
		// However, for any free axes, allow an arbitrary rotation which better matches hand's current placement.
		CPlacement3<> p3 = pmp_pickup->pr3Presence();

		if (pmp_pickup->pmagData->setFlags[emfXFREE])
			AdjustPickup(&p3, d3XAxis, pmp_pickup, p3_palm);
		if (pmp_pickup->pmagData->setFlags[emfYFREE])
			AdjustPickup(&p3, d3YAxis, pmp_pickup, p3_palm);
		if (pmp_pickup->pmagData->setFlags[emfZFREE])
			AdjustPickup(&p3, d3ZAxis, pmp_pickup, p3_palm);

		return p3;
	}

	//*****************************************************************************************
	//
	inline CVector3<> v3HeadOrigin()
	//
	// Returns:
	//		Origin to use for hand positioning.
	//
	//**********************************
	{
		return CVector3<>(0, 0, PlayerSettings.v3HeadOffset.tZ);
	}

	//*****************************************************************************************
	//
	bool bPickupable
	(
		CInstance* pins
	)
	//
	// Returns:
	//		Whether this is the type of thing we can pick up.
	//
	//**********************************
	{
		// Can only grab movable objects.
		if (!pins->pphiGetPhysicsInfo()->bIsMovable())
			return false;

		// Find what this is magneted to. This will catch attempt to pick up stowed object.
		pins = NMagnetSystem::pinsFindMaster(pins);

		if (pins->pphiGetPhysicsInfo()->fMass(pins) >= PlayerSettings.fMaxMassPickup)
			return false;

		// Check for sliding magnets. Not allowed to grab them, as the physics don't work.
		if (NMagnetSystem::pmpFindMagnet(pins, Set(emfSLIDE)))
			return false;

		// Cannot not grab terrain, water, myself, or my own or any other boundary box,
		// or a night out with the neighbour.
		return	!ptCast<CTerrain>(pins) && !ptCast<CEntityWater>(pins) && 
				!ptCast<CAnimate>(pins) && !ptCast<CBoundaryBox>(pins);
	}

	//*****************************************************************************************
	//
	bool bCloseEnough
	(
		const CPlacement3<>& p3_a,
		const CPlacement3<>& p3_b,
		TSec s_elapsed,
		bool b_check_orient = false
	)
	//
	// Returns:
	//		Whether these two placements are close enough for pickup.
	//
	//**********************************
	{
		// Otherwise, check a distance.
		// We start with a threshold half the maximim, and linearly increase with time.
		float f_frac = 0.5 + 0.5 * s_elapsed / PlayerSettings.sPickupTime;
		TReal r_dist_pickup = PlayerSettings.fHandDistPickup * f_frac;
		Assert(r_dist_pickup >= 0);

		TReal r_dist_sqr = (p3_a.v3Pos - p3_b.v3Pos).tLenSqr();
		if (r_dist_sqr <= Sqr(r_dist_pickup))
		{
			if (!b_check_orient)
				return true;

			// Check rotation as well.
			TReal r_min_cos = 1.0 - PlayerSettings.rHandRotPickup * f_frac;
			CRotate3<> r3_diff = p3_a.r3Rot / p3_b.r3Rot;
			if (Abs(r3_diff.tC) >= r_min_cos)
				return true;
		}

		return false;
	}
};

//**********************************************************************************************
//
class CVRLeftArm: public CAnimate
//
// Prefix: vrla
//
// VR mod: a render-only copy of Anne's skinned mesh, drawn mirrored, posed so that only its
// arm shows (at the off-hand controller).  It is only in the world while the VR view renders.
//
//**************************************
{
public:
	bool bInWorld = false;

	CVRLeftArm(const CInstance::SInit& initins)
		: CAnimate(initins)
	{
		// Purely visual: no game logic, physics or collisions.
		CMessagePhysicsReq::UnregisterRecipient(this);
		 CMessageCollision::UnregisterRecipient(this);
		      CMessageStep::UnregisterRecipient(this);
		bSkeletonUpdated = true;
	}
};

//**********************************************************************************************
//
class CPlayerPriv: public CPlayer
//
// Prefix: play
//
// Implementation class for CPlayer.
//
//**************************************
{
private:

	//
	// Implementation data.
	//

	// Ouch sounds for normal damage.
	CMSArray<float, 5>		afOuchDamageLevels;	// The level of damage at which we say ouch.
	CMSArray<TSampleSet, 5>	assOuchSampleSets;	// How we say ouch at each level.
	TSampleSet				ssOuchDefault;		// What we say when we have nothing better to say.

	// Ouch sounds for falling damage.
	CMSArray<float, 5>		afOuchFallDamageLevels;	// The level of damage at which we say ouch.
	CMSArray<TSampleSet, 5>	assOuchFallSampleSets;	// How we say ouch at each level.
	TSampleSet				ssOuchFallDefault;		// What we say when we have nothing better to say.

	
	TSampleSet				ssJump;				// What we say when we have nothing better to say.

#define iMAX_NUMERICAL_BULLET_COUNT 18			// 0 to 17 bullets left

	// Ammo count down sounds.
	CMSArray<TSampleSet, iMAX_NUMERICAL_BULLET_COUNT>	assAmmo;	// How we count ammo.
	TSampleSet				ssAmmo20;								// Sounds for 20 shots.
	TSampleSet				ssAmmo30;								// Sounds for 30 shots.
	TSampleSet				ssAmmoAlmostEmpty;				
	TSampleSet				ssAmmoHalfFull;
	TSampleSet				ssAmmoFull;

	// Ammo pickup count sounds.
	CMSArray<TSampleSet, iMAX_NUMERICAL_BULLET_COUNT>	assPickup;	// How we count ammo.
	TSampleSet				ssPickup20;								// Sounds for 20 shots.
	TSampleSet				ssPickup30;								// Sounds for 30 shots.
	TSampleSet				ssPickupAlmostEmpty;				
	TSampleSet				ssPickupHalfFull;
	TSampleSet				ssPickupFull;							// 75%-99%
	TSampleSet				ssPickupReallyFull;						// 100%

	CRandom rnd;			// Random number generator for all player functions.

	CPArray<const CPhysicsInfoBox*> 
					papphibHandBoxes;	// List of hand box physics infos for each substitute mesh
										// (special feature only on player).

	CRotate3<>		r3Body;		// Desired orientation of body (independent of physics system).
	CPlacement3<>	p3Head;		// Placement of head relative to body (no instance).
	CPlacement3<>	p3Hand;		// Desired placement of hand, in world space.

	CPlacement3<>	p3Stowed,	// Remembered hand placement for stowed object,
					p3Stowing;	// and for stowing object.
	CPlacement3<>	ap3StowedObj[2];
								// Actual relative stowed positions of objects.
								// Used to eliminate drift when dragging.

	CInstance*		pinsHand;	// Attached hand instance.

	enum EHandActivity
	// Prefix: eha
	{
		ehaINACTIVE,			// Hand down at player's side.
		ehaRAISING,				// Hand is activated, but is being raised.
		ehaACTIVE				// Hand is activated and ready.
	};
	EHandActivity	eHandActivity;
	TSec			sRaiseTime;	// Time at which hand will be raised.

	enum EHandHolding
	// Prefix: ehh
	{
		ehhEMPTY,				// Not holding or trying to hold anything.
		ehhSEEKING,				// Searching for objects to grab.
		ehhGRABBING,			// Moving toward a pickup position for holding.
		ehhGRABBED,				// Contacted the target object.
		ehhHOLDING,				// Actually holding an object.
		ehhDROPPING				// Just dropped the object.
	};
	EHandHolding	eHandHolding;
	CInstance*		pinsHeld;		// Object the player is currently holding, if any.
	CInstance*		apinsStowed[2];	// Object the player has stowed in each slot, if any.
	int				iStowSlot;		// Slot the object is stowed in, -1 if none.
	int				iStowing;		// Stowing stage we are currently in (0 is none).
	int				iSwinging;		// Swinging stage we are currently in (0 is none).
	CVector2<>		v2AnglesDraw;	// Player-relative angle to draw stowed object to.

	bool			bHandRotate;	// Hand is currently rotated.
	bool			bAutoCrouching;	// Forced crouching when picking up objects on the ground.
	bool			bThrowing;		// In process of quick-throw.
	TSec			sActionTime;	// Time at which next action will be completed.
	CVector2<>		v2Throw;		// Accumulated throw velocity.

	int				iSubstitute;		// Current substitution mesh/box.
	int				iSubstituteRequest;	// Current requested substitution.
										// Will substitute later if unable to do so now.

	int				iScreenFlashTime;	// Screen flash frame count down.
	float			angShakeAmplitude;	// Current camera vibration intensity.
										// Decreases exponentially.


	// Vocal variables.
	TSec			sDoneTalking;		// Time stamp of the time we'll finish our "effects" vocal
	uint32			u4AudioID;			// The ID of the vocalization currently being made by Anne.
										//  Zero if none.

	//**************************************************************************************
	//
	// Constructors and destructor.
	//

public:

	//**************************************************************************************
	CPlayerPriv();

private:

	virtual ~CPlayerPriv();

	//**************************************************************************************
	//
	// Member functions.
	//

	//*****************************************************************************************
	//
	CVector3<> v3HandToPalmNormal
	(
		const CPlacement3<>& p3_hand
	) const
	//
	// Returns:
	//		Palm position given hand placement.
	//
	//**********************************
	{
		return (PlayerSettings.v3WristToPalmNormal - apbbBoundaryBoxes[(int)ebbHAND]->v3GetOffset()) * p3_hand;
	}

	//*****************************************************************************************
	//
	CVector3<> v3HandToPalmSpecial
	(
		const CPlacement3<>& p3_hand
	) const
	//
	// Returns:
	//		Palm position given hand placement.
	//
	//**********************************
	{
		return (PlayerSettings.v3WristToPalmSpecial - apbbBoundaryBoxes[(int)ebbHAND]->v3GetOffset()) * p3_hand;
	}

	//*****************************************************************************************
	//
	CVector3<> v3PalmToHandNormal
	(
		const CPlacement3<>& p3_palm
	) const
	//
	// Returns:
	//		Hand box position given normal palm placement.
	//
	//**********************************
	{
		return -(PlayerSettings.v3WristToPalmNormal - apbbBoundaryBoxes[(int)ebbHAND]->v3GetOffset()) * p3_palm;
	}

	//*****************************************************************************************
	//
	CVector3<> v3PalmToHandSpecial
	(
		const CPlacement3<>& p3_palm
	) const
	//
	// Returns:
	//		Hand box position given special pickup magnet palm placement.
	//
	//**********************************
	{
		return -(PlayerSettings.v3WristToPalmSpecial - apbbBoundaryBoxes[(int)ebbHAND]->v3GetOffset()) * p3_palm;
	}

	//*****************************************************************************************
	//
	CVector3<> v3PalmToWristNormal
	(
		const CPlacement3<>& p3_palm
	) const
	//
	// Returns:
	//		Wrist joint position given normal palm placement.
	//
	//**********************************
	{
		return -PlayerSettings.v3WristToPalmNormal * p3_palm;
	}

	//*****************************************************************************************
	//
	CVector3<> v3PalmToWristSpecial
	(
		const CPlacement3<>& p3_palm
	) const
	//
	// Returns:
	//		Wrist joint position given special pickup magnet palm placement.
	//
	//**********************************
	{
		return -PlayerSettings.v3WristToPalmSpecial * p3_palm;
	}

	//*****************************************************************************************
	//
	CPlacement3<> p3HandActual() const
	//
	// Returns:
	//		Actual wrist centre placement in body space.
	//
	//**********************************
	{
		return pinsHand->p3GetPlacement() / p3GetPlacement();
	}

	//*****************************************************************************************
	//
	CVector3<> v3HandActual() const
	//
	// Returns:
	//		Actual wrist centre position in body space.
	//
	//**********************************
	{
		return pinsHand->p3GetPlacement().v3Pos / p3GetPlacement();
	}


	//*****************************************************************************************
	//
	CVector3<> v3Shoulder() const
	//
	// Returns:
	//		The position of the player's right shoulder.
	//
	//**********************************
	{
		return sriBones.v3JointPos(10) * pr3GetPresence().rScale;
	}

	//*****************************************************************************************
	//
	CVector3<> v3Sight() const;
	//
	// Returns:
	//		The sighting position on a held object, or the hand if none.
	//
	//**********************************

	//*****************************************************************************************
	//
	static CVector3<> v3HandBody
	(
		CAngles2 ang2_hand,
		TReal r_extend
	)
	//
	// Returns:
	//		Hand position given angles and extent, in body space.
	//
	//**********************************
	{
		return d3FromAngles(ang2_hand) * r_extend + v3HeadOrigin();
	}

	//*****************************************************************************************
	//
	CVector3<> v3HandShoulder
	(
		CAngles2 ang2_hand,
		TReal r_extend
	) const
	//
	// Returns:
	//		Hand position given angles and extent, in shoulder space.
	//
	//**********************************
	{
		return d3FromAngles(ang2_hand) * r_extend + v3Shoulder();
	}

	//*****************************************************************************************
	//
	TReal rHandDistance() const;
	//
	// Returns:
	//		The correct distance of the hand from the origin.
	//
	//**********************************

	//*****************************************************************************************
	//
	void SetHandPosHead
	(
		CAngles2 ang2_hand_head,		// Desired angles of hand relative to head.
		TReal r_extend = 0.0			// If default, computes default extension.
	);
	//
	// Sets target hand position in body space, given angles and extent.
	//
	//**********************************

	//*****************************************************************************************
	//
	void SetHandPosShoulder
	(
		CAngles2 ang2_hand_shoulder,	// Desired angles of hand relative to shoulder, in body space.
		TReal r_extend = 0.0			// If default, computes default extension.
	);
	//
	// Sets target hand position in body space, given angles and extent.
	//
	//**********************************

	//*****************************************************************************************
	//
	void AdjustHandDist
	(
		TReal r_dist
	);
	//
	// Moves the hand target to the given shoulder distance.
	//
	//**********************************

	//*****************************************************************************************
	//
	static CAngles2 ang2HandBody
	(
		const CVector3<>& v3_hand
	)
	//
	// Returns:
	//		Corresponding angles of v3 in body space (body rotation, head offset).
	//
	//**********************************
	{
		return Angles2(CDir3<>(v3_hand - v3HeadOrigin()));
	}

	//*****************************************************************************************
	//
	CAngles2 ang2HandShoulder
	(
		const CVector3<>& v3_hand
	) const
	//
	// Returns:
	//		Corresponding angles of v3 in shoulder space (body rotation, shoulder offset).
	//
	//**********************************
	{
		return Angles2(CDir3<>(v3_hand - v3Shoulder()));
	}

	//*****************************************************************************************
	//
	CRotate3<> r3HandRequestSpace() const
	//
	// Returns:
	//		The space in which hold orientations are requested.
	//
	//**********************************
	{
		return CRotate3<>(d3YAxis, CDir3<>(v3Sight() - p3Head.v3Pos));
	}

	//*****************************************************************************************
	//
	inline CRotate3<> r3HandLimitSpace(const CVector3<>& v3_hand) const
	//
	// Returns:
	//		The space in which hand rotation limits are applied (shoulder relative).
	//
	//**********************************
	{
		return r3OrientSpace(v3_hand - v3Shoulder());
	}

	//*****************************************************************************************
	//
	void SetHead
	(
		CRotate3<> r3_head
	);
	//
	// Sets player's head from angles.
	//
	//**********************************

	//*****************************************************************************************
	//
	CPlacement3<> p3NormalPickupHand(const SObjectLoc& obl, const CPlacement3<>& p3_palm_cur)
	//
	// Returns:
	//		The hand placement for picking up this object, in world space.
	//
	//**********************************
	{
		// Pick up at raycast intersection. Init orientation to hand's current orientation.
		CPlacement3<> p3_pickup = CPlacement3<>(p3_palm_cur.r3Rot, obl.v3Location);

		// Face the hand back (Z) to the object normal. 
		// Use shortest (great circle) rotation possible.
		p3_pickup.r3Rot = p3_palm_cur.r3Rot * CRotate3<>(d3ZAxis * p3_palm_cur.r3Rot, obl.d3Face);

#if bHINGE_ADJUST
		// Offset to get equivalent hand position.
		if (NMagnetSystem::pmpFindMagnet(obl.pinsObject, Set(emfHINGE)))
			//
			// We special case hinge objects to compensate for physics behaviour.
			// The physics will place the bio-mesh wrist at the desired hand position.
			// Therefore, we must return the position we want the wrist at, rather than the hand.
			//
			p3_pickup.v3Pos = v3PalmToWristNormal(p3_pickup);
		else
#endif
			p3_pickup.v3Pos = v3PalmToHandNormal(p3_pickup);

		return p3_pickup;
	}

	//*****************************************************************************************
	//
	CPlacement3<> p3SpecialPickupHand
	(
		const CMagnetPair* pmp_pickup
	)
	//
	// Returns:
	//		The world pickup placement for this object.
	//
	//**********************************
	{
		// Get current target hand position.
		CPlacement3<> p3_hand_target = p3Hand;
		p3_hand_target.r3Rot *= r3HandRequestSpace();
		p3_hand_target *= p3GetPlacement();

		// Determine pickup position from target palm position.
		CPlacement3<> p3_hand_pickup = p3SpecialPickup(pmp_pickup, p3_hand_target);

		// Offset to get equivalent hand position.
#if bHINGE_ADJUST
		if (NMagnetSystem::pmpFindMagnet(pmp_pickup->pinsMaster, Set(emfHINGE)))
			//
			// We special case hinge objects to compensate for physics behaviour.
			// The physics will place the bio-mesh wrist at the desired hand position.
			// Therefore, we must return the position we want the wrist at, rather than the hand.
			//
			p3_hand_pickup.v3Pos = v3PalmToWristSpecial(p3_hand_pickup);
		else
#endif
			p3_hand_pickup.v3Pos = v3PalmToHandSpecial(p3_hand_pickup);
		return p3_hand_pickup;
	}

	//*****************************************************************************************
	//
	float angShakeIntensity() const;
	//
	// Returns:
	//		The angular roll to use due to current camera shake.
	//
	//**********************************

	//*****************************************************************************************
	//
	void ClipHandRotate();
	//
	// Clips the hand rotation to given limits.
	//
	//**********************************

	//*****************************************************************************************
	//
	void SetHandRotate();
	//
	// Sets player's hand rotation from held object.
	//
	//**********************************

	//*****************************************************************************************
	//
	bool bHoldingFreeObj() const
	//
	// Returns:
	//		Whether we are holding an object that is free (not magneted to world).
	//
	//**********************************
	{
		return eHandHolding == ehhHOLDING
			&& !NMagnetSystem::pmpFindMagnet(pinsHeld, Set(emfFREEZE) + emfHINGE + emfSLIDE);
	}

	//*****************************************************************************************
	//
	bool bSwingable() const;
	//
	// Returns:
	//		Whether the held object can be swung.
	//
	//**********************************

	//*****************************************************************************************
	//
	int iStowable() const;
	//
	// Returns:
	//		In which slot the currently held object may be stowed.
	//		0 for side, 1 for back, -1 for unstowable.
	//
	//**********************************

	//*****************************************************************************************
	//
	void Stow();
	//
	// Swaps any held object with the stowed object.
	// All objects must be in desired position: hand, held object, stowed object.
	//
	//**********************************

	//*****************************************************************************************
	//
	void ProtectHand
	(
		bool b_ignore					// How to protect the hand.
	);
	//
	// Protects the hand from the harsh environment of the physical world, by checking if it
	// currently intersects any other objects.
	//
	// If so, then if b_ignore, the hand temporarily ignores intersecting objects (until it's free).
	// If not b_ignore, then the hand is moved to a safe place in the body.
	//
	// Called just after activating or moving the hand.
	//
	//**********************************

	//*****************************************************************************************
	//
	void AttemptSubstitute
	(
		int i_sub						// Substitution index.
	);
	//
	// Attempt to actually substitute the requested mesh index.
	//
	//**********************************

	//*****************************************************************************************
	//
	bool bHandFollowHead() const;
	//
	// Returns:
	//		Whether the hand should follow the head's movements.
	//
	//**********************************

	//*****************************************************************************************
	//
	void Jostle
	(
		float f_intensity
	)
	//
	//**********************************
	{
		// Jostle the player by setting the pelvis command.
		// Create a random direction perturbed from Z axis up to 45 degrees.
		CDir3<> d3_intensity(CRandom::randMain(-0.5, 0.5), CRandom::randMain(-0.5, 0.5), 1.0);
		CVector3<> v3_intensity = d3_intensity * f_intensity;

		Pelvis_Jump[0] = v3_intensity.tX;
		Pelvis_Jump[1] = v3_intensity.tY;
		Pelvis_Jump[2] = v3_intensity.tZ;
		Pelvis_Jump_Voluntary = false;
	}

	//*****************************************************************************************
	//
	void Say
	(
		TSoundHandle sndhnd
	);
	//
	// Speak the sample.
	//
	//**************************************

	//*****************************************************************************************
	//
	void Say
	(
		const TSampleSet* pss
	);
	//
	// Pick a member of the set, and say it.
	//
	//**************************************

	//
	// Special control handlers.
	//

	//*****************************************************************************************
	void CheckWristDrop();

	//*****************************************************************************************
	void HandleDrop(const CMessageControl& msgc);

	//*****************************************************************************************
	void HandleThrow(const CMessageControl& msgc);

	//*****************************************************************************************
	void HandleStow(const CMessageControl& msgc, int i_vr_slot = -1);

	//
	// VR mod: both hands.  [0] = right, [1] = left.
	//
	struct SVRHand
	{
		CInstance*    pinsBox = nullptr;		// Physics box that pushes things (owned by the world).
		bool          bBoxInWorld = false;
		CInstance*    pinsHeld = nullptr;		// Held object (kinematic, exactly at the controller).
		CPlacement3<> p3Grab;					// Held object relative to the hand.
		CInstance*    pinsReleased = nullptr;	// Recently released (still ignores Anne's body).
		TSec          sReleaseTime = -100;
		bool          bPrevGrip = false;
		bool          bPrevTrigger = false;
		CInstance*    pinsIgnoreOther = nullptr;	// The other hand's object, which this box ignores.
		CInstance*    pinsForceTarget = nullptr;
		TSec          sForcePullTime = -100;
	};
	SVRHand avrh[2];

	bool bVRHands() const
	{
		return gVR.bActive && gVR.bMotionHand;
	}
	CPlacement3<> p3VRController(int i_hand) const;
	void HandleVRHand(int i_hand);
	void HandleForceGrab(int i_hand);
	void ReleaseVR(int i_hand, bool b_throw);
	bool bVRHolster(int i_hand, int i_slot);
	void GrabVR(int i_hand, CInstance* pins, const CPlacement3<>& p3_hand);
	void IgnoreVR(int i_hand, CInstance* pins, bool b_ignore);
	void PoseVRArm(CSkeletonRenderInfo& sri, const CTransform3<>* atf3, int i_transforms,
	               rptr_const<CBioMesh> pbm, bool b_mirror, CPlacement3<> p3_target, float f_curl);
	void BodyFollow();

	// Visual second arm (a mirrored copy of Anne's mesh).
	CVRLeftArm*    pvrlaLeftArm = nullptr;
	rptr<CRenderType> prdtLeftArmMesh;
	TSec           sVRBrakeUntil = -100;
	bool           bVRBodyShifted = false;
	CPresence3<>   pr3VRRealPresence;

	//*****************************************************************************************
	void HandleSwing(const CMessageControl& msgc, bool b_use);


	//
	// CPlayer implementation.
	//

	//*****************************************************************************************
	virtual void Update
	(
		const CGroffObjectName*	pgon,		// Object to load.
		CLoadWorld*				pload,		// The loader.
		const CHandle&			h_object,	// handle of object in value table.
		CValueTable*			pvtable,	// Value Table.
		const CInfo*			pinfo		// The info to copy.  Create a new one if 0.
	) override
	{
		// Woo.  Hellish.
		Assert(!pinfo);

		// Load the inferior mesh and attach it.
		rptr<CRenderType> prdt_player = CRenderType::prdtFindShared
		(
			pgon,				
			pload,				
			h_object,
			pvtable
		);

		SetRenderInfo(prdt_player);
		SetPhysicsInfo(new CPhysicsInfoPlayer(this));

		SetPresence(::pr3Presence(*pgon));

		// Get hit points, regen stats, boundary boxes.
		CAnimate::ParseProps(pgon, pload, h_object, pvtable, pinfo);

		// Copy the hand box to our commonly-used member.
		pinsHand = apbbBoundaryBoxes[(int)ebbHAND];
		Assert(pinsHand);

		// Turn on these facilities, as a convenience when you add a player model.
		// Note: done before checking hand box, as activation creates default boxes.
		bPhysics = true;
		pphiGetPhysicsInfo()->Activate(this);

		// Player has been previously set to "no shadow" because she had no mesh.  Override that here.
		bool b_cast_shadow = true;

		// Load boundary boxes for substitution meshes.
		rptr<CRenderType> prdt = prdtGetRenderInfoWritable();
		rptr<CSubBioMesh> psbm = rptr_dynamic_cast(CSubBioMesh, prdt);
		if (psbm)
		{
			// Allocate array of hand boxes.
			new(&papphibHandBoxes) CAArray< rptr_const<CPhysicsInfo> >(psbm->lpsubSubstitutes.size());
			papphibHandBoxes.Fill(0);

			// Set hand 0 to our original hand box.
			papphibHandBoxes[0] = pinsHand->pphiGetPhysicsInfo()->ppibCast();

			SETUP_TEXT_PROCESSING(pvtable, pload)
			SETUP_OBJECT_HANDLE(h_object)

			bFILL_BOOL(b_cast_shadow, esShadow);
			
			// Iterate through substitution objects, to find attached boxes.
			const CEasyString* pestr_sub = 0;
			for (int i_sub = 0; bFILL_pEASYSTRING(pestr_sub, ESymbol(esA00 + i_sub)); i_sub++)
			{
				Assert(pestr_sub);
				CGroffObjectName* pgon_sub = pload->goiInfo.pgonFindObject(pestr_sub->strData());

				if (pgon_sub && pgon_sub->hAttributeHandle != 0)
				{
					SETUP_OBJECT_HANDLE(pgon_sub->hAttributeHandle)
			
					// For now, look only for hand box.
					const CEasyString* pestr_box = 0;
					if (bFILL_pEASYSTRING(pestr_box, esHand))
					{
						CGroffObjectName* pgon_box = pload->goiInfo.pgonFindObject(pestr_box->strData());
						if (pgon_box)
						{
							// Read the instance from the loader.
							CInstance* pins_temp = new CInstance(pgon_box, pload, pgon_box->hAttributeHandle, pvtable, 0);

							// Adjust the box to main hand's scale.
							TReal r_scale = pins_temp->fGetScale() / pinsHand->fGetScale();
							CPhysicsInfoBox* pphib = pins_temp->pphiGetPhysicsInfoWritable()->ppibCast();

							pphib->bvbBoundVol *= r_scale;
							pphib->bvbCollideVol *= r_scale;
							pphib->v3Pivot *= r_scale;

							// Boxes must be tangible, movable.
							AlwaysAssert(pphib->bIsMovable() && pphib->bIsTangible());

							// Attempt to fix it if we're retrying.
							pphib->setFlags[epfMOVEABLE][epfTANGIBLE] = true;

							// Store it away permanently, via FindShared.
							// Keep the physics, and wantonly discard the rest.
							papphibHandBoxes[i_sub+1] = CPhysicsInfoBox::pphibFindShared(*pphib);

							delete pins_temp;
						}
						else
						{
							// Missing animal box!!!
							dout << "Missing player hand box (" << pestr_box->strData() << ")\n";
						}
					}

					END_OBJECT;
				} // if (pgon_sub)
			}

			END_OBJECT;
			END_TEXT_PROCESSING;
		}

		// Copy hand instance, for convenient reference.
		if (pinsHand)
			Assert(pinsHand == apbbBoundaryBoxes[(int)ebbHAND])
		else
			pinsHand = apbbBoundaryBoxes[(int)ebbHAND];

		// And use that shadow flag.
		SetFlagShadow(b_cast_shadow);
	}

	//*****************************************************************************************
	virtual void UpdatePlayerSettings
	(
		const CGroffObjectName*	pgon,		// Object to load.
		CLoadWorld*				pload,		// The loader.
		const CHandle&			h_object,	// handle of object in value table.
		CValueTable*			pvtable,	// Value Table.
		const CInfo*			pinfo		// The info to copy.  Create a new one if 0.
	) override
	{

#define PARSE_SAMPLE_SET(ss)\
		for (int i_samples = 0; i_samples < 8; ++i_samples)\
		{\
			const CEasyString* pestr = 0;\
			if (bFILL_pEASYSTRING(pestr, ESymbol(esA00 + i_samples)))\
			{\
				ss << sndhndHashIdentifier(pestr->strData());\
			}\
			else\
			{\
				break;\
			}\
		}

		// Snag the names of the head, body. and tail boxes.
		SETUP_TEXT_PROCESSING(pvtable, pload)
		SETUP_OBJECT_HANDLE(h_object)
		{
			// Handle the Ouch noises.
			IF_SETUP_OBJECT_SYMBOL(esOuch)
			{
				// Now loop through the array entries
				for (int i_sets = 0; i_sets < 8; ++i_sets)
				{
					IF_SETUP_OBJECT_SYMBOL(ESymbol(esA00 + i_sets))
					{
						// Found a sample set!
						float f_damage = 0.0;
						bool b = bFILL_FLOAT(f_damage, esDamage);
						if (b)
						{
							// Found a damage level!  Add to the ouch level arrays.
							TSampleSet ss;

							PARSE_SAMPLE_SET(ss);
					
							if (ss.uLen > 0)
							{
								// Found some samples!
								afOuchDamageLevels << f_damage;
								assOuchSampleSets << ss;
							}
							else
							{
								AlwaysAssert(false);
							}	
						}
						else
						{
							PARSE_SAMPLE_SET(ssOuchDefault);
					
							if (ssOuchDefault.uLen > 0)
							{
							}
							else
							{
								AlwaysAssert(false);
							}	
						}
					}
					END_OBJECT;
				}
			}
			END_OBJECT;

			// Handle the Falling Ouch noises.
			IF_SETUP_OBJECT_SYMBOL(esFall)
			{
				// Now loop through the array entries
				for (int i_sets = 0; i_sets < 8; ++i_sets)
				{
					IF_SETUP_OBJECT_SYMBOL(ESymbol(esA00 + i_sets))
					{
						// Found a sample set!
						float f_damage = 0.0;
						bool b = bFILL_FLOAT(f_damage, esDamage);
						if (b)
						{
							// Found a damage level!  Add to the ouch level arrays.
							TSampleSet ss;

							PARSE_SAMPLE_SET(ss);
					
							if (ss.uLen > 0)
							{
								// Found some samples!
								afOuchFallDamageLevels << f_damage;
								assOuchFallSampleSets << ss;
							}
							else
							{
								AlwaysAssert(false);
							}	
						}
						else
						{
							PARSE_SAMPLE_SET(ssOuchFallDefault);
					
							if (ssOuchFallDefault.uLen > 0)
							{
							}
							else
							{
								AlwaysAssert(false);
							}	
						}
					}
					END_OBJECT;
				}
			}
			END_OBJECT;

			
			IF_SETUP_OBJECT_SYMBOL(esAmmo)
			{
				// Now loop through the array entries
				for (int i_sets = 0; i_sets < iMAX_NUMERICAL_BULLET_COUNT; ++i_sets)
				{
					IF_SETUP_OBJECT_SYMBOL(ESymbol(esA00 + i_sets))
					{
						PARSE_SAMPLE_SET(assAmmo[i_sets]);
					}
					END_OBJECT;
				}

				IF_SETUP_OBJECT_SYMBOL(ESymbol(esA00 + 20))
				{
					PARSE_SAMPLE_SET(ssAmmo20);
				}
				END_OBJECT;

				IF_SETUP_OBJECT_SYMBOL(ESymbol(esA00 + 30))
				{
					PARSE_SAMPLE_SET(ssAmmo30);
				}
				END_OBJECT;

				IF_SETUP_OBJECT_SYMBOL(esAmmoAlmostEmpty)
				{
					PARSE_SAMPLE_SET(ssAmmoAlmostEmpty);
				}
				END_OBJECT;

				IF_SETUP_OBJECT_SYMBOL(esAmmoHalfFull)
				{
					PARSE_SAMPLE_SET(ssAmmoHalfFull);
				}
				END_OBJECT;

				IF_SETUP_OBJECT_SYMBOL(esAmmoFull)
				{
					PARSE_SAMPLE_SET(ssAmmoFull);
				}
				END_OBJECT;
			}
			END_OBJECT;

			IF_SETUP_OBJECT_SYMBOL(esAmmoPickup)
			{
				// Now loop through the array entries
				for (int i_sets = 0; i_sets < iMAX_NUMERICAL_BULLET_COUNT; ++i_sets)
				{
					IF_SETUP_OBJECT_SYMBOL(ESymbol(esA00 + i_sets))
					{
						PARSE_SAMPLE_SET(assPickup[i_sets]);
					}
					END_OBJECT;
				}

				IF_SETUP_OBJECT_SYMBOL(ESymbol(esA00 + 20))
				{
					PARSE_SAMPLE_SET(ssPickup20);
				}
				END_OBJECT;

				IF_SETUP_OBJECT_SYMBOL(ESymbol(esA00 + 30))
				{
					PARSE_SAMPLE_SET(ssPickup30);
				}
				END_OBJECT;

				IF_SETUP_OBJECT_SYMBOL(esAmmoAlmostEmpty)
				{
					PARSE_SAMPLE_SET(ssPickupAlmostEmpty);
				}
				END_OBJECT;

				IF_SETUP_OBJECT_SYMBOL(esAmmoHalfFull)
				{
					PARSE_SAMPLE_SET(ssPickupHalfFull);
				}
				END_OBJECT;

				IF_SETUP_OBJECT_SYMBOL(esAmmoFull)
				{
					PARSE_SAMPLE_SET(ssPickupFull);
				}
				END_OBJECT;

				IF_SETUP_OBJECT_SYMBOL(esAmmoReallyFull)
				{
					PARSE_SAMPLE_SET(ssPickupReallyFull);
				}
				END_OBJECT;
			}
			END_OBJECT;

			IF_SETUP_OBJECT_SYMBOL(esJumpUp)
			{
				PARSE_SAMPLE_SET(ssJump);
			}
			END_OBJECT;

		}
		END_OBJECT;
		END_TEXT_PROCESSING;
	}

	//*****************************************************************************************
	virtual CInstance* pinsHeldObject() const override
	{
		if (bVRHands())
			return avrh[0].pinsHeld ? avrh[0].pinsHeld : avrh[1].pinsHeld;
		return bWithin(eHandHolding, ehhHOLDING, ehhDROPPING) ? pinsHeld : 0;
	}
	
	//*****************************************************************************************
	virtual bool bHandInteract(const CInstance* pins) const override
	{
		if (eHandActivity == ehaINACTIVE && eHandHolding < ehhHOLDING)
			// Hand is inactive, and thus interacts with NOTHING, do you hear?
			return false;
		else if (eHandHolding < ehhHOLDING)
			// Hand is active, but not holding anything.
			// It interacts with everything but the foot.
			return pins != apbbBoundaryBoxes[(int)ebbFOOT];
		else
			// Otherwise, it interacts as normal, even with the body.
			return true;
	}
	
	//*****************************************************************************************
	virtual void MaybeTalkAboutAmmo(int i_current, int i_max) override
  	{
		// Bad, Bad, Bad, No negative ammo counts please.
		if (i_current < 0)
			i_current = 0;

		// Only speak every third time.  Do the rand first to avoid replay problems.
		// Is Anne already speaking?
		Assert(padAudioDaemon);
		if (!(rnd() % 3) || !bCanTalk())
			return;

		if (i_current < iMAX_NUMERICAL_BULLET_COUNT)
		{
			Say(&assAmmo[i_current]);
		}
		else if (i_current == 20)
		{
			Say(&ssAmmo20);
		}
		else if (i_current == 30)
		{
			Say(&ssAmmo30);
		}
	}

	//*****************************************************************************************
	virtual void TalkAboutApproximateAmmo(int i_sample) override
  	{
		// ALWAYS say approximates, because they happen so rarely.

		// Is Anne already speaking?
		Assert(padAudioDaemon);
		if (!bCanTalk())
			return;

		if (i_sample == 0)
		{
			Say(&ssAmmoAlmostEmpty);
		}
		else if (i_sample == 1)
		{
			Say(&ssAmmoHalfFull);	 // Anne is an optimist.
		}
		else if (i_sample == 2)
		{
			Say(&ssAmmoFull);
		}
	}

	//*****************************************************************************************
	virtual void TalkAboutAmmoOnPickup
	(
		int i_current, 
		int i_maximum, 
		bool b_approximate
	) override
	{
		// Bad, Bad, Bad, No negative ammo counts please.
		if (i_current < 0)
			i_current = 0;

		Assert(i_maximum > 0);

		// Good place for ammo count.
		if (b_approximate && i_current)
		{
			// We have some ammo, give an approximation.

			// < 30%
			if (i_current < i_maximum*0.30f)
			{
				Say(&ssPickupAlmostEmpty);
			}
			// < 75%
			else if (i_current < i_maximum*0.75f)
			{
				Say(&ssPickupHalfFull);
			}
			// 75%-99%
			else if (i_current < i_maximum)
			{
				Say(&ssPickupFull);
			}
			// 100%
			else
			{
				Say(&ssPickupReallyFull);
			}
		}
		else
		{
			// Exact count if we can.
			if (i_current <= 10)
			{
				Say(&assPickup[i_current]);
			}
			else if (i_current >= 11 && i_current <= 13)
			{
				Say(&assPickup[12]);
			}
			else if (i_current >= 14 && i_current <= 17)
			{
				Say(&assPickup[15]);
			}
			else if (i_current >= 18 && i_current <= 25)
			{
				Say(&ssPickup20);
			}
			else if (i_current >= 26 && i_current <= 35)
			{
				Say(&ssPickup30);
			}
		}
	}

	//*****************************************************************************************
	virtual bool bCanTalk() override
	{
	    // Is Anne already speaking?
		Assert(padAudioDaemon);

		// Are we voiceovering?
		if (padAudioDaemon->bVoiceOver)
			// Yes!  Please keep quiet.
			return false;

		// If we are done talking, then we can start again.
		return CMessageStep::sStaticTotal > sDoneTalking;
	}

	//*****************************************************************************************
	virtual void StopTalking() override
	{
		// Kill all voiceovers and music.
		AlwaysAssert(padAudioDaemon);
		padAudioDaemon->KillVoiceovers();
		padAudioDaemon->RemoveSubtitle();

		sDoneTalking = -10.0f;

		if (u4AudioID)
		{		
			// Send the stop message.
			StopSoundEffect(u4AudioID);
		}
		u4AudioID = 0;
	}

	//*****************************************************************************************
	virtual void JumpSound() override
	{
		Say(&ssJump);
	}

	//*****************************************************************************************
	virtual void Pickup(CInstance* pins_obj) override
	{


		// VR: an object can only be in one hand.
		for (int i = 0; i < 2; i++)
			if (pins_obj && pins_obj == avrh[i].pinsHeld)
				ReleaseVR(i, false);

		// If there is a preference, set the hand mesh.
		// Must be done before magneting, so new hand box takes effect.
		const CMagnetPair* pmp_pickup = NMagnetSystem::pmpFindMagnet(pins_obj, Set(emfHAND_HOLD));
		if (pmp_pickup && pmp_pickup->pmagData->u1SubstituteMesh)
		{
			Substitute(pmp_pickup->pmagData->u1SubstituteMesh);
		}
		else
		{
			// If not, make sure that we are using the default hand.
			Substitute(0);
		}

		eHandHolding = ehhHOLDING;
		pinsHeld = pins_obj;

		// Turn off any ignoring for this pair; not needed.
		pphSystem->IgnoreCollisions(pinsHand, pinsHeld, false);

		// Pick up the just-touched object.  The hand is the master, naturally.
		NMagnetSystem::AddMagnetPair(pinsHand, pinsHeld, CMagnet::pmagFindShared(CMagnet()));

		// Set the hand rotation from its hold magnet.
		SetHandRotate();

		// If no hold magnet, but there is a pickup magnet, restore default rotation
		// (as Anne may have contorted her hand painfully to get the object).
		// Otherwise, preserve current rotation.
		if (!pmp_pickup)
			bHandRotate = true;

		// Set the time to wait before allowing physics drop.
		sRaiseTime = CMessageStep::sStaticTotal + PlayerSettings.sPickupNoDrop;

		// Do the pickup action for the item.
		pins_obj->PickedUp();

		// Send the pickup message
		CMessagePickUp(pins_obj, epuPICKUP).Dispatch();
	}

	//*****************************************************************************************
	virtual void Drop(CVector3<> v3_throw = v3Zero) override
	{
		// VR: drop what pinsHeldObject() reports (scripts and sockets call this).
		if (bVRHands())
		{
			for (int i = 0; i < 2; i++)
				if (avrh[i].pinsHeld)
				{
					CInstance* pins = avrh[i].pinsHeld;
					ReleaseVR(i, false);
					if (!v3_throw.bIsZero())
						pins->pphiGetPhysicsInfoWritable()->ForceVelocity(pins, v3_throw);
					return;
				}
		}

		// See if we're holding.
		if (!bWithin(eHandHolding, ehhHOLDING, ehhDROPPING))
			return;
		Assert(pinsHeld);

		//
		// Drop.
		//

		NMagnetSystem::RemoveMagnets(pinsHand);

		// Ignore immediate collisions between the hand and the dropped object.
		pphSystem->IgnoreCollisions(pinsHand, pinsHeld, true);

		if (pphSystem->bIgnoreCollisions(pinsHand, apbbBoundaryBoxes[(int)ebbBODY]))
		{
			// If the hand is currently ignoring the body, we must make the released
			// object ignore the body as well.
			// Very important to avoid self-wounding.
			pphSystem->IgnoreCollisions(pinsHeld, apbbBoundaryBoxes[(int)ebbBODY], true);
			pphSystem->IgnoreCollisions(pinsHeld, apbbBoundaryBoxes[(int)ebbFOOT], true);
		}

		if (!v3_throw.bIsZero())
		{
			// The impulse point is midway between the palm and the object centre, for both objects.
			CVector3<> v3_loc = 
				v3HandToPalmNormal(pinsHand->p3GetPlacement()) * PlayerSettings.fThrowTorque + 
				pinsHeld->v3Pos() * (1.0 - PlayerSettings.fThrowTorque);

			// Clamp the magnitude.
			TReal r_len = v3_throw.tLen();
			if (r_len > PlayerSettings.fThrowMomentumScale)
				v3_throw *= (PlayerSettings.fThrowMomentumScale / r_len);

			// Apply momentum to hand and object.
			// Apportion it according to masses.
			float f_mass_held = pinsHeld->pphiGetPhysicsInfo()->fMass(pinsHeld);
			float f_mass_hand = pinsHand->pphiGetPhysicsInfo()->fMass(pinsHand);

			// Physics currently uses a minimum mass of 1.
			SetMax(f_mass_held, 1.0);
			SetMax(f_mass_hand, 1.0);

			// Clamp for velocity.
			TReal r_vel = v3_throw.tLen() / f_mass_held;
			if (r_vel > PlayerSettings.fThrowVelocityMax)
				v3_throw *= PlayerSettings.fThrowVelocityMax / r_vel;
			dout << "Throw velocity = " <<r_vel <<"\n";

			pinsHeld->pphiGetPhysicsInfo()->ApplyImpulse(pinsHeld, 0, v3_loc, v3_throw);
			pinsHand->pphiGetPhysicsInfo()->ApplyImpulse(pinsHand, 0, v3_loc, v3_throw * (f_mass_hand / (f_mass_hand + f_mass_held)));
		}

		// Send a put down message
		CMessagePickUp(pinsHeld, epuDROP).Dispatch();

		eHandHolding = ehhEMPTY;
		pinsHeld = 0;
		bHandRotate = false;

		// Return to default hand mesh.
		// Do it after dropping, so it will be allowed.
		Substitute(0);
	}

	//*****************************************************************************************
	virtual void CheckForDrop() override
	{
		// See if we're holding.
		if (!PlayerSettings.bAllowDrop)
			return;

		// Check only if we're holding something and/or the hand is active.
		if (!bWithin(eHandHolding, ehhGRABBING, ehhHOLDING) && eHandActivity == ehaINACTIVE)
			return;
		Assert(pinsHeld);

		// Get current local placements of hand and shoulder in physics (not world) system.
		int i_body = pphSystem->iGetIndex(apbbBoundaryBoxes[(int)ebbBODY]);
		Assert(i_body >= 0);
		CPlacement3<> p3_pel = CPhysicsInfoBox::p3BoxElement(i_body, 0);

		int i_hand = pphSystem->iGetIndex(pinsHand);
		Assert(i_hand >= 0);
		CPlacement3<> p3_hand = CPhysicsInfoBox::p3BoxElement(i_hand, 0) / p3_pel;

		// Drop if our arm is too long.
		TReal r_len_max;
		if (eHandHolding == ehhGRABBING || CMessageStep::sStaticTotal < sRaiseTime)
			// Still picking up. Use max grab distance, and add an extra tolerance
			// to avoid rapid pickup/drop sequence.
			r_len_max = PlayerSettings.fHandReachGrab * 1.2f;
		else
			// Holding. Use max reach distance, to provide a margin beyond max extension.
			r_len_max = PlayerSettings.fHandReachSpecial;

		TReal r_len = (p3_hand.v3Pos - v3Shoulder()).tLen();
		if (r_len > r_len_max)
		{
			dout <<"Drop due to length: " <<r_len <<" [" <<r_len_max <<"]\n";
			Drop();

			// At this point, let's also deactivate the hand, so it's sure to come back to us.
			// If it intersects an object on the way, it will shoot to its safe place.
			eHandActivity = ehaINACTIVE;
		}
	}

	//*****************************************************************************************
	virtual void DrawPhysics(CDraw& draw, CCamera& cam) const override
	{
#if bVER_BONES()
		CTransform3<> tf3_screen = cam.tf3ToHomogeneousScreen();
		CTransform3<> tf3_player_screen = p3GetPlacement() * tf3_screen;

		if (eHandActivity > ehaINACTIVE ||
			eHandHolding >= ehhHOLDING ||
			bHandRotate)
		{
			// Draw desired hand pos.
			CPlacement3<> p3_hand = p3Hand;
			p3_hand.r3Rot *= r3HandRequestSpace();

			CPresence3<> pr3_hand(p3_hand * p3GetPlacement(), pinsHand->fGetScale());
			CTransform3<> tf3_hand_screen = pr3_hand * tf3_screen;

			draw.CoordinateFrame3D(tf3_hand_screen, 1.5, 0.03 / pinsHand->fGetScale());

			// Draw desired physics hand box as well.
			if (bHandRotate)
			{
				CTransform3<> tf3_box_screen = pinsHand->pbvBoundingVol()->tf3Box(tf3_hand_screen);
				draw.Colour(CColour(1.0, 0.5, 0.5));
				draw.Box3D(tf3_box_screen);

				// Draw line from head to line-of-sight.
				draw.Line3D(p3Head.v3Pos * tf3_player_screen, v3Sight() * tf3_player_screen);
			}
		}

		// Draw head box.
		draw.Colour(CColour(1.0, 0.5, 0.5));

		CBoundVolBox bvb_head(0.2f, 0.1f, 0.3f);
		CTransform3<> tf3_box = bvb_head.tf3Box(p3Head * tf3_player_screen);
		draw.Box3D(tf3_box);
#endif
	}

	//*****************************************************************************************
	//
	// CAnimate overrides.
	//

	//*****************************************************************************************
	virtual bool bIncludeInBuildPart() const override
	{
		return false;
	}

	//*****************************************************************************************
	virtual void HandleDamage(float f_damage, const CInstance* pins_aggressor, const CInstance* pins_me) override
	{
		if (gVR.pfnLog)
			gVR.pfnLog("Damage: %.1f to %s from %s (health now %.1f of %.1f).", f_damage,
				pins_me ? pins_me->strGetInstanceName() : "?",
				pins_aggressor ? pins_aggressor->strGetInstanceName() : "?", fHitPoints, fMaxHitPoints);

		// What were my hit points before?
		Assert(fMaxHitPoints > 0.0f);
		float f_inv_max			= 1.0f / fMaxHitPoints;
		float f_normalized		= fHitPoints * f_inv_max;

		// Show some visual feedback for damage.
		if (prasMainScreen->bHasGamma())
			prasMainScreen->GammaFlash(1.0f - f_normalized);
		else
			Overlay.Enable(1.0f - f_normalized);

		// This result in flash persisting for one frame.
		iScreenFlashTime = 2;

		// Are we dead?
		if (fHitPoints < 0)
		{
			// Yes!  Did we just die?
			if (fHitPoints + f_damage > 0)
			{
				// Yes!  This is the killing blow.  Stop talking and make your death speech.
				StopTalking();
			}
			else
			{
				// No.  We were already dead.  Don't speak.
				return;
			}
		}

	    // Is Anne already speaking?
		Assert(padAudioDaemon);
		if (!bCanTalk())
			return;

		TSoundHandle sndhnd = 0;

		float f_lowest_higher = 2.0f;	// lowest value higher than the normalized HP

		// Figure out whether we were falling.
		bool b_falling = (pins_me == apbbBoundaryBoxes[(int)ebbFOOT]);
		if (b_falling)
		{
			// Check each sound threshold.
			for (int i = 0; i < afOuchFallDamageLevels.uLen; ++i)
			{
				// Have we bracketed the damage level?
				if (f_normalized < afOuchFallDamageLevels[i] && f_lowest_higher > afOuchFallDamageLevels[i])
				{
					// We found an appropriate sample set!
					TSampleSet* pss = &assOuchFallSampleSets[i];

					// Verify that we have at least one sample here.
					AlwaysAssert(pss->uLen > 0);

					// Snag the handle.
					sndhnd = (*pss)[rnd() % pss->uLen];

					// And set the lowest higher value.
					f_lowest_higher = afOuchFallDamageLevels[i];
				}
			}
		}

		// Do we have a good sound handle now?
		if (!sndhnd)
		{
			// No!  Go ahead and use normal sounds.
			// Check each sound threshold.
			for (int i = 0; i < afOuchDamageLevels.uLen; ++i)
			{
				// Have we bracketed the damage level?
				if (f_normalized < afOuchDamageLevels[i] && f_lowest_higher > afOuchDamageLevels[i])
				{
					// We found an appropriate sample set!
					TSampleSet* pss = &assOuchSampleSets[i];

					// Verify that we have at least one sample here.
					AlwaysAssert(pss->uLen > 0);

					// Snag the handle.
					sndhnd = (*pss)[rnd() % pss->uLen];

					// And set the lowest higher value.
					f_lowest_higher = afOuchDamageLevels[i];
				}
			}
		}

		// Not speaking!  Say a boring ouch.
		if (!sndhnd)
		{
			if (b_falling && ssOuchFallDefault.uLen > 0)
				sndhnd = ssOuchFallDefault[rnd() % ssOuchFallDefault.uLen];
			else if (ssOuchDefault.uLen > 0)
				sndhnd = ssOuchDefault[rnd() % ssOuchDefault.uLen];
		}

		// Finally, say it again, Anne!
		if (sndhnd)
		{
			Say(sndhnd);
		}
	}

	//*****************************************************************************************
	virtual float fWieldDamage(const CInstance* pins_weapon, float f_coll_damage) const override
	{
		if (iSwinging == 2)
			f_coll_damage *= PlayerSettings.fSwingDamageMul;
		return f_coll_damage;
	}

	//*****************************************************************************************
	virtual void Substitute(int i_sub) override
	{
		// Save our requested substitution.
		// Actually perform the substitution next control message.

		// Cannot substitute non-default mesh while holding.
		if (bWithin(eHandHolding, ehhHOLDING, ehhDROPPING))
			return;

		iSubstituteRequest = i_sub;
	}

	//*****************************************************************************************
	virtual CPlacement3<> p3HeadPlacement() const override
	{
		return p3Head;
	}

	//*****************************************************************************************
	virtual void VRShowLeftArm(bool b_show) override;

	//*****************************************************************************************
	//
	// Overrides.
	//

	//******************************************************************************************
	virtual void Move(const CPlacement3<>& p3_new, CEntity* pet_sender) override
	{
		if (pet_sender == pphSystem)
		{
			// Updated from physics.

			// Preserve head's world rotation, and hand's target position relative to player position.
			CRotate3<> r3_adjust = p3GetPlacement().r3Rot / p3_new.r3Rot;
			bool b_adjusted = r3_adjust.v3S.tX != 0.0f ||
								r3_adjust.v3S.tY != 0.0f ||
								r3_adjust.v3S.tZ != 0.0f;

			if (b_adjusted)
			{
				if (bWithin(eHandHolding, ehhGRABBING, ehhGRABBED))
				{
					// If grabbing, maintain world target hand placement, even if body moves, crouches, etc.
					p3Hand *= p3GetPlacement() / p3_new;
				}
				else if (eHandActivity > ehaINACTIVE)
					// If player is controlling the hand, maintain world hand position relative to body.
					p3Hand.v3Pos *= r3_adjust;
			}

			if (bVRHead())
			{
				// VR: head orientation comes from the headset, relative to the new body facing.
				CAngles2 ang2_vr = ang2VRHead(p3_new.r3Rot);
				SetMinMax(ang2_vr.tX, -dPI_2 * 0.95, dPI_2 * 0.95);
				SetMinMax(ang2_vr.tY, -dPI_2 * 0.95, dPI_2 * 0.95);
				SetHead(r3FromAngles(ang2_vr));
			}
			else
			{
			// Otherwise, maintain body-relative position.
			CRotate3<> r3_head;
			if (b_adjusted)
			{
				r3_head = p3Head.r3Rot * r3_adjust;
			}
			else
			{
				r3_head = p3Head.r3Rot;
			}

			CAngles2 ang2_head = Angles2(r3_head);
			CAngles2 ang2_start = ang2_head;

			if (eHandActivity >= ehaACTIVE && !iSwinging && !iStowing && !bThrowing)
			{
				//
				// If palm has moved too far, track with head.
				//

				// Get hand angles in head space, and apply delta.
				CPlacement3<> p3_hand = PlayerSettings.bHeadFollowActual ? 
					p3HandActual() : p3Hand;
				CAngles2 ang2_diff = ang2HandBody(v3HandToPalmNormal(p3_hand)) - ang2_head;

				if (eHandActivity == ehaACTIVE)
				{
					//
					// Move X only if not dropping,
					//
					if (ang2_diff.tX < PlayerSettings.ang2HandViewMin.tX)
						ang2_head.tX += ang2_diff.tX - PlayerSettings.ang2HandViewMin.tX;
					else if (ang2_diff.tX > PlayerSettings.ang2HandViewMax.tX)
						ang2_head.tX += ang2_diff.tX - PlayerSettings.ang2HandViewMax.tX;
				}

				if (ang2_diff.tY < PlayerSettings.ang2HandViewMin.tY)
					ang2_head.tY += ang2_diff.tY - PlayerSettings.ang2HandViewMin.tY;
				else if (ang2_diff.tY > PlayerSettings.ang2HandViewMax.tY)
					ang2_head.tY += ang2_diff.tY - PlayerSettings.ang2HandViewMax.tY;
			}

			// Whether we adjusted head or not, limit it based on current body position.
			SetMinMax(ang2_head.tX, -PlayerSettings.ang2HeadViewMax.tX, PlayerSettings.ang2HeadViewMax.tX);
			SetMinMax(ang2_head.tY, -PlayerSettings.ang2HeadViewMax.tY, PlayerSettings.ang2HeadViewMax.tY);

			if (ang2_head.tX != ang2_start.tX || ang2_head.tY != ang2_start.tY)
			{
				// Orientation has changed!
				r3_head = r3FromAngles(ang2_head);
			}

			// Add the camera shake and roll.
			CAngle ang = angShakeIntensity();
			//if (angShakeAmplitude > 0.000001)
			r3_head = CRotate3<>(d3YAxis, ang) * r3_head;

			SetHead(r3_head);
			} // !bVRHead()
		}
		else
		{
			// Moved outside of physics. Check for uprightness.
			if (p3_new.r3Rot.v3S.tX || p3_new.r3Rot.v3S.tY)
			{
				CPlacement3<> p3_adjust = p3_new;

				// Right the rotation.  The Z axis must be up.
				CDir3<> d3_z = d3ZAxis * p3_new.r3Rot;
				if (d3_z.tZ < 0.99)
					p3_adjust.r3Rot *= CRotate3<>(d3_z, d3ZAxis);
				p3_adjust.r3Rot.v3S.tX = p3_adjust.r3Rot.v3S.tY = 0.0;

				CAnimate::Move(p3_adjust, pet_sender);
				return;
			}
		}

		CAnimate::Move(p3_new, pet_sender);
	}

	//*****************************************************************************************
	virtual void Process(const CMessageSystem& msgsys) override
	{
		if (msgsys.escCode == escSTOP_SIM)
		{
			// Turn off gamma flash.
			if (iScreenFlashTime > 0)
			{
				// Remove gamma flash or disable overlay.
				if (prasMainScreen->bHasGamma())
					prasMainScreen->RestoreGamma();
				else
					Overlay.Disable();
			}
		}
	}

	//*****************************************************************************************
	virtual void Process(const CMessageStep& msgstep) override
	{
		// Invoke base class message handling.
		CAnimate::Process(msgstep);

		// Show animated texture health meter.
		if (prdtGetRenderInfo())
		{
			//  HACK HACK HACK
			// We do this horrible thing because rpdtCopy() is not implemented, and if it were, we'd be needlessly
			// copying meshes in this case-  animated meshes are both shared and modifiable.  This way, we avoid calling
			// prdtGetRenderInfoWritable on a non-biomesh.
			rptr<CMesh> pmsh;

			if (pinsAttached)
			{
				pmsh = ptCastRenderType<CMesh>(pinsAttached->prdtGetRenderInfo());
			}

			if (!pmsh || !pmsh->bIsAnimated())
			{
				pmsh = ptCastRenderType<CMesh>(prdtGetRenderInfo());
			}

			if (pmsh && pmsh->bIsAnimated())
			{
				CMeshAnimating* pma = (CMeshAnimating*)pmsh.ptGet();

				float f_inv_max		= 1.0f / fMaxHitPoints;
				float f_normalized	= fHitPoints * f_inv_max;
				int i_len			= pma->aptexTextures.size();
				if (i_len)
				{
					// Don't use the last frame unless we are at zero.
					int i_frame = (i_len - 1) * (1.0f - f_normalized);

					// Clamp to 0..1-length.
					if (i_frame < 0)
						i_frame = 0;
					if (i_frame >= i_len)
						i_frame = i_len-1;

					pma->SetFrame(i_frame);
				}
			}
		}

		if (iScreenFlashTime > 0)
		{
			iScreenFlashTime--;

			if (iScreenFlashTime == 0)
			{
				// Remove gamma flash or disable overlay.
				if (prasMainScreen->bHasGamma())
					prasMainScreen->RestoreGamma();
				else
					Overlay.Disable();
			}
		}

		// Decay the camera shake.
		angShakeAmplitude *= exp(-fShakeDecay * msgstep.sStep);

		// Activate any triggers in the region by sending out an "Is Here" message.
//		CMessageHere msghere(this);
//		msghere.Dispatch();
#if bCAMERA_DEATH
		// Do the camera-death thing here.
		if (bDead())
		{
			// Get the camera associated with the player.
			CCamera* pcam = CWDbQueryActiveCamera().tGet();
			if (pcam && pcam->pinsAttached() == this)
			{
				const float dTARGET_ANGLE = dDegreesToRadians(135);	// Target angle.
				const float dGROWTH_RATE  = 0.025;					// Rate of approach to target angle.

				// Incrementally approach the target angle.
				CCamera::SProperties camprop = pcam->campropGetProperties();

				double d_angle = camprop.angGetAngleOfView();
				d_angle += (dTARGET_ANGLE - d_angle) * dGROWTH_RATE;
				if (d_angle > dTARGET_ANGLE)
					d_angle = dTARGET_ANGLE;
				camprop.SetAngleOfView(d_angle);

				pcam->SetProperties(camprop);
			}
		}
#endif
	}

	//*****************************************************************************************
	virtual void Process(const CMessageControl& msgc) override
	{
		bool b_use = false;

		// Do not accept control messages while an animation is in progress.
		if (!bControl)
			return;

		if (bDead())
		{
			// Keep it short and sweet...You're dead. Be thankful you can even move your head.
			CAngles2 ang2_head = Angles2(p3Head.r3Rot);
			if (bVRHead())
				ang2_head = ang2VRHead(p3GetPlacement().r3Rot);
			else
				ang2_head += msgc.v2Rotate;

			// Update p3Head here directly.
			SetHead(r3FromAngles(ang2_head));
			return;
		}

		// Handle the use command here.
		if (msgc.bPressed(uCMD_USE))
		{
			// Are we holding an object?
			if (eHandHolding == ehhHOLDING)
			{
				// Yes!  USE it.
				Assert(pinsHeld);

				// If no use occurred, we still need to perform the default action.
				b_use = !pinsHeld->bUse(!msgc.bHit(uCMD_USE));

				// Send a message for an object being used..
				CMessageUse	msguse(pinsHeld);
				msguse.Dispatch();
			}
			else
			{
				// Use when empty.
				b_use = true;
			}
		}

		// Only act when enabled.
		if (!bPhysics)
			return;

		// Make sure our hero is active.
		PhysicsActivate();

		// Where we place the physics commands.
		CMessagePhysicsReq msgpr;

		const CRating rt_high_urgency(.9f);
		const CRating rt_low_urgency(.2f);

		// Set all urgencies to 0.
		msgpr.Reset();

		CAngles2 ang2_body = Angles2(p3GetPlacement().r3Rot);
		CAngles2 ang2_head = Angles2(p3Head.r3Rot);

		// Let the VR input mapping know whether the trigger should grab or use.
		for (int i = 0; i < 2; i++)
		{
			CInstance* pins = avrh[i].pinsHeld;
			gVR.abHolding[i]    = pins != nullptr;
			gVR.abHoldingGun[i] = pins && ptCast<CGun>(pins) != 0;
			gVR.afHeldMass[i]   = pins ? pins->pphiGetPhysicsInfo()->fMass(pins) : 0.0f;
		}
		if (gVR.bActive)
		{
			// Holster locations, for the VR mod's proximity test.
			for (int i = 0; i < 2; i++)
			{
				CVector3<> v3 = PlayerSettings.aap3Stow[i][2].v3Pos * p3GetPlacement();
				gVR.aafHolsterPosWorld[i][0] = v3.tX;
				gVR.aafHolsterPosWorld[i][1] = v3.tY;
				gVR.aafHolsterPosWorld[i][2] = v3.tZ;
			}
		}

		//
		// Perform any delayed substitution.
		//

		AttemptSubstitute(iSubstituteRequest);

		//
		// Handle hand pickup and drop.
		//

		if (eHandHolding < ehhHOLDING)
		{
			//
			// Documentation of pickup sequence:
			//
			// Each frame: If eHandHolding < ehhHOLDING and grab pressed:
			//		If eHandHolding == ehhEMPTY:
			//			eHandHolding = ehhSEEKING.
			//		If eHandHolding == ehhSEEKING:
			//			Raycast for special pickup object. If found:
			//				If too low, auto-crouch.
			//				eHandHolding = ehhGRABBING.
			//				Mark object for pickup, ignore collisions between hand and object,
			//				start moving toward its pickup.
			//			If we collided with an object (tested elsewhere in collision handler):
			//				eHandHolding = ehhGRABBED.
			//				Mark object for pickup.
			//		If eHandHolding == ehhGRABBING:
			//			Test whether hand has reached pickup location.
			//			If so: Pick it up (eHandHolding = ehhHOLDING).
			//		If eHandHolding == ehhGRABBED:
			//			Determine ideal pickup point on object we collided with.
			//			Pick it up (eHandHolding = ehhHOLDING).
			//

			// Player not yet holding an object.
			if (eHandActivity >= ehaACTIVE && msgc.bPressed(uCMD_GRAB) && iSubstitute == 0)
			{
				// Hand is fully raised, grab control is on, physics drop semaphore is off, and we have default hand mesh. 
				// Attempt to pick up object.
				SetMax(eHandHolding, ehhSEEKING);

				if (eHandHolding == ehhGRABBED)
				{
					//
					// We just collided with a box.
					// Determine ideal pickup location on box, and pick it up.
					//

					Assert(pinsHeld);

					// Check for special pickup magnet.
					const CMagnetPair* pmp_pickup;
					if (pmp_pickup = NMagnetSystem::pmpFindMagnet(pinsHeld, Set(emfHAND_HOLD)))
					{
						// Find pickup position.
						CPlacement3<> p3_hand_pickup = p3SpecialPickupHand(pmp_pickup);
						CVector3<> v3_pickup = p3_hand_pickup.v3Pos / p3GetPlacement();

						TReal r_dist = (v3_pickup - v3Shoulder()).tLen();
						if (r_dist <= PlayerSettings.fHandReachGrab)
						{
							// Pick this up as if we had raycast to it.
							eHandHolding = ehhGRABBING;

							// Fraudulently back-set the action time, so we pick up quicker after colliding.
							sActionTime = CMessageStep::sStaticTotal - 0.5;
						}
						else
						{
							// Won't pick it up, still seeking.
							eHandHolding = ehhSEEKING;
							pinsHeld = 0;
						}
					}
					else
					{
						// Raycast from current palm to hit object centre.
						CPlacement3<> p3_palm = pinsHand->p3GetPlacement();
						p3_palm.v3Pos = v3HandToPalmNormal(p3_palm);

						// Find nearest point on box; use a "diameter" of 0.1 m,
						// which scales the amount of preference given to rotation.
						CRayCast rc(p3_palm, 0.1, 0.0);

						// Make sure we found the object.
						SObjectLoc* pobl;
						while (pobl = rc.poblNextIntersection())
						{
							if (pobl->pinsObject == pinsHeld)
								break;
						}

						// We'd better have found it.
						if (Verify(pobl))
						{
							// Determine pickup position from pobl and p3_palm.
							CPlacement3<> p3_hand_pickup = p3NormalPickupHand(*pobl, p3_palm);

							CPlacement3<> p3_pickup_play = p3_hand_pickup / p3GetPlacement();

							// Only allow pickup if required hand rotation is within limits.
							// (this should cull pickups based on back-of-hand collision).
							CRotate3<> r3_cmd = p3_pickup_play.r3Rot / r3HandLimitSpace(p3_pickup_play.v3Pos);

							// Check angles in command space.
							CAngles3 ang3_cmd = Angles3(r3_cmd);

							if (bWithin(ang3_cmd.tX, PlayerSettings.ang3HandRotMin.tX, PlayerSettings.ang3HandRotMax.tX) &&
								bWithin(ang3_cmd.tY, PlayerSettings.ang3HandRotMin.tY, PlayerSettings.ang3HandRotMax.tY) &&
								bWithin(ang3_cmd.tZ, PlayerSettings.ang3HandRotMin.tZ, PlayerSettings.ang3HandRotMax.tZ))
							{
								// Set corresponding placement in player space for hand command.
								p3Hand = p3_pickup_play;
								p3Hand.r3Rot /= r3HandRequestSpace();

								// Let's force the hand to the exact position we wanted.
								pinsHand->Move(p3_hand_pickup);

								// Pick up the object.
								Pickup(pobl->pinsObject);
							}
							else
							{
								// Won't pick it up, still seeking.
								eHandHolding = ehhSEEKING;
								pinsHeld = 0;
							}
						}
					}
				} // if (eHandHolding == ehhGRABBED)

				if (eHandHolding == ehhSEEKING)
				{
					// Look for an object to pick up.
					Assert(!pinsHeld);

					// What are we seeking? Perform a raycast to find out, from the head to the actual palm.
					CVector3<> v3_head = p3Head.v3Pos * p3GetPlacement();

					// Use actual palm position because it's more visually intuitive
					CVector3<> v3_palm = v3HandToPalmNormal(pinsHand->p3GetPlacement());
					CDir3<> d3_cast = v3_palm - v3_head;
					CPlacement3<> p3_cast(CRotate3<>(d3YAxis, d3_cast), v3_head);

					//
					// Cast with an extended reach, so we're sure to catch objects we might
					// need to crouch to get.
					//
					float f_reach = Max(PlayerSettings.fHandDistMax, PlayerSettings.fHandReachSpecial);
					CRayCast rc(p3_cast, PlayerSettings.fHandReachWidth, 2.0*f_reach);

					// Find first grabbable object in path.
					SObjectLoc* pobl;
					while (pobl = rc.poblNextIntersection())
					{
						if (bPickupable(pobl->pinsObject))
							break;
					}

					if (pobl)
					{
						//
						// Check shoulder distance.
						//
						CVector3<> v3_pickup;				// Pickup position in player space.
						TReal r_max_dist;					// Max allowed shoulder distance.
						bool b_allow_crouch;				// Whether we can auto-crouch for this object.

						// Determine pickup position from target palm position.
						const CMagnetPair* pmp_pickup = NMagnetSystem::pmpFindMagnet(pobl->pinsObject, Set(emfHAND_HOLD));
						if (pmp_pickup)
						{
							// Find pickup position.
							CPlacement3<> p3_hand_pickup = p3SpecialPickupHand(pmp_pickup);
							v3_pickup = p3_hand_pickup.v3Pos / p3GetPlacement();

							// Larger distance for picking up special objects.
							r_max_dist = PlayerSettings.fHandReachSpecial;
							b_allow_crouch = PlayerSettings.bAutoCrouchSpecial;

							// Tentatively start grabbing this object (pending distance check).
							eHandHolding = ehhGRABBING;
						}
						else
						{
							// Use the raycast pickup location.
							v3_pickup = pobl->v3Location / p3GetPlacement();
							r_max_dist = PlayerSettings.fHandDistMax;
							b_allow_crouch = PlayerSettings.bAutoCrouchNormal;
						}

						TReal r_dist = (v3_pickup - v3Shoulder()).tLen();
						if (r_dist > r_max_dist)
						{
							// Too far away.
							// Enable auto-crouch when we aim for any object, even without pickup.
							// This won't necessarily be the object we pick up, but we'll crouch just the same.
							if (b_allow_crouch && !(bAutoCrouching || msgc.bPressed(uCMD_CROUCH)))
							{
								// Can't reach it from here. See if we can reach it crouching.
								CVector3<> v3_shoulder_crouch = v3Shoulder();
								v3_shoulder_crouch.tZ -= PlayerSettings.fCrouchDist;

								// If when crouching, it is within reach, and below head height, crouch.
								if (v3_pickup.tZ < v3_shoulder_crouch.tZ &&
									(v3_shoulder_crouch - v3_pickup).tLenSqr() <= Sqr(r_max_dist))
								{
									// We can reach it crouching, and must auto-crouch!
									bAutoCrouching = true;

									// Set the time when we expect crouch to be finished.
									sActionTime += PlayerSettings.sCrouchDelay;
								}
								else
									// We can't reach this object. Keep looking.
									eHandHolding = ehhSEEKING;
							}
							else
								// Keep looking.
								eHandHolding = ehhSEEKING;
						}

						// Check whether to latch onto this object.
						if (eHandHolding == ehhGRABBING)
						{
							// We are still grabbing.
							// Latch on to this object now. 
							// Pick it up when our hand reaches target position.
							// Do not perform subsequent raycasts while grabbing.
							pinsHeld = pobl->pinsObject;

							// Remember the pickup start time.
							sActionTime = CMessageStep::sStaticTotal;
						}
					} // if (pobl)
				} // if (eHandHolding == ehhSEEKING)

				if (eHandHolding == ehhGRABBING)
				{
					// See whether we can pick up yet.
					if (pinsHeld)
					{
						//
						// We have a target object with special pickup. 
						// Redetermine pickup position, and pick up when hand gets there.
						//

						// Determine pickup position from target palm position.
						const CMagnetPair* pmp_pickup = NMagnetSystem::pmpFindMagnet(pinsHeld, Set(emfHAND_HOLD));
						Assert(pmp_pickup);
						CPlacement3<> p3_hand_pickup = p3SpecialPickupHand(pmp_pickup);

						// Convert back to player request space.
						p3Hand = p3_hand_pickup / p3GetPlacement();
						p3Hand.r3Rot /= r3HandRequestSpace();

						// Set reasonable limits.
						ClipHandRotate();
						bHandRotate = true;

						// Reduce reach when waiting for auto-crouch.
						if (bAutoCrouching && CMessageStep::sStaticTotal < sActionTime)
						{
							CVector3<> v3_hand_head = p3Hand.v3Pos - p3Head.v3Pos;
							TReal r_len = v3_hand_head.tLen();
							if (r_len > PlayerSettings.fHandDistMax)
							{
								// Reduce reach, and don't rotate hand yet.
								v3_hand_head *= PlayerSettings.fHandDistMax / r_len;
								p3Hand.v3Pos = v3_hand_head + p3Head.v3Pos;
								bHandRotate = false;
							}
						}
						else
						{
							//
							// Check for pickup.
							//
							TSec s_elapsed = CMessageStep::sStaticTotal - sActionTime;
							if (s_elapsed >= PlayerSettings.sPickupTime ||
								bCloseEnough(pinsHand->p3GetPlacement(), p3_hand_pickup, s_elapsed))
							{
								// Let's force the hand to the exact position we wanted.
								pinsHand->Move(p3_hand_pickup);

								// Pick up the object.
								Pickup(pinsHeld);
							}
							else
								// During grabbing, ignore collisions between these objects.
								pphSystem->IgnoreCollisions(pinsHand, pinsHeld, true);
						}
					} // if (pinsHeld)
				} // if (eHandHolding == ehhGRABBING)
			} // if grabbing
			else
			{
				// Not grabbing. Back to empty.
				eHandHolding = ehhEMPTY;
				pinsHeld = 0;
			}
		} // if not holding

		//
		// Handle special commands.
		//

		HandleDrop(msgc);
		HandleThrow(msgc);
		if (!bVRHands())
			HandleStow(msgc);

		for (int i = 0; i < 2; i++)
		{
			HandleForceGrab(i);
			HandleVRHand(i);
		}
		HandleSwing(msgc, b_use);

		//
		// Perform non-grabbing hand movement.
		//

		TReal r_extend = 0.0;

		if (iStowing || bThrowing)
			// Disable all hand commands.
			;
		else if (msgc.bPressed(uCMD_HAND) || eHandHolding == ehhDROPPING)
		{
			// Check for hand raise.
			if (PlayerSettings.bHeadFollowActual && 
				eHandActivity == ehaRAISING)
			{
				// Get hand angles in head space, and apply delta.
				CAngles2 ang2_palm_actual = ang2HandBody(v3HandActual());
				ang2_palm_actual -= ang2_head;

				// See whether hand is in view yet, or whether enough time has passed.  
				// If so, it's fully active, and head will track.
				if ((bWithin
				(
					ang2_palm_actual.tX,
					PlayerSettings.ang2HandViewMin.tX, 
					PlayerSettings.ang2HandViewMax.tX
				) && bWithin
				(
					ang2_palm_actual.tY, 
					PlayerSettings.ang2HandViewMin.tY, 
					PlayerSettings.ang2HandViewMax.tY
				))/* || CMessageStep::sStaticTotal >= sRaiseTime*/)
					eHandActivity = ehaACTIVE;

				// VR: the hand is wherever the controller is; no need to see it first.
				if (gVR.bActive && gVR.bMotionHand)
					eHandActivity = ehaACTIVE;
			}

			if (!bWithin(eHandHolding, ehhGRABBING, ehhGRABBED))
			{
				// Get hand extent.
				if (b_use || msgc.bPressed(uCMD_PUNCH) || eHandHolding == ehhSEEKING ||
					(eHandHolding == ehhEMPTY && msgc.bPressed(uCMD_GRAB)))
				{
					// If punching, or seeking an object, or "using" empty hand, extend it.
					r_extend = PlayerSettings.fHandDistMax;
				}
				else
				{
					// Use distance appropriate to held object.
					r_extend = rHandDistance();

#if bDROP_THROW
					if (eHandHolding == ehhDROPPING)
					{
						// Set extent based on current magnitude of throw Y.
						// Full cock is half regular extent.
						TReal r_throw_mag = Abs(v2Throw.tY);
						Assert(r_throw_mag <= 1.0);
						r_extend *= 1.0 - 0.5 * r_throw_mag;
					}
#endif
				}

				CVector3<> v3_hand = p3Hand.v3Pos;

				if (eHandActivity > ehaINACTIVE ||
					eHandHolding == ehhDROPPING)
				{
					if (!msgc.bPressed(uCMD_SHIFT) && !msgc.bPressed(uCMD_CONTROL))
					{
						//
						// Get hand angles in body space, and apply delta.
						//
						CAngles2 ang2_hand = ang2HandShoulder(v3_hand);

						// Apply mouse rotation to hand position.
#if bDROP_THROW
						if (eHandHolding == ehhDROPPING)
						{
							// Move in X only. 
							ang2_hand.tX += msgc.v2Rotate.tX;
						}
						else
#endif
							ang2_hand += msgc.v2Rotate;

						{
							// If hand moves too far, drag the body.
							if (!bWithin(ang2_hand.tX, -PlayerSettings.ang2HandTurnMax.tX, PlayerSettings.ang2HandTurnMax.tX))
							{
								// Hand has swung around too much, rotate the body.
								if (ang2_hand.tX < 0)
									ang2_body.tX += Max(ang2_hand.tX - -PlayerSettings.ang2HandTurnMax.tX, -dPI_2 * 0.95);
								else 
									ang2_body.tX += Min(ang2_hand.tX -  PlayerSettings.ang2HandTurnMax.tX,  dPI_2 * 0.95);

								CRotate3<> r3_body_new = CRotate3<>(d3ZAxis, CAngle(-ang2_body.tX));
								msgpr.subOrientBody.Set(rt_low_urgency, 1, r3_body_new);
							}

							// Clamp the hand X angle, reducing Y delta proportionally.
							if (ang2_hand.tX < -PlayerSettings.ang2HandMax.tX)
							{
								// Clamp the hand X angle, reducing Y delta proportionally.
								float f_reduce = msgc.v2Rotate.tX ? 
									(-PlayerSettings.ang2HandMax.tX - ang2_hand.tX + msgc.v2Rotate.tX) / msgc.v2Rotate.tX
									: 0;
								ang2_hand.tX = -PlayerSettings.ang2HandMax.tX;
								if (msgc.v2Rotate.tY)
									ang2_hand.tY -= (1.0 - f_reduce) * msgc.v2Rotate.tY;
							}
							else if (ang2_hand.tX > PlayerSettings.ang2HandMax.tX)
							{
								float f_reduce = msgc.v2Rotate.tX ? 
									(PlayerSettings.ang2HandMax.tX - ang2_hand.tX + msgc.v2Rotate.tX) / msgc.v2Rotate.tX
									: 0;
								ang2_hand.tX = PlayerSettings.ang2HandMax.tX;
								if (msgc.v2Rotate.tY)
									ang2_hand.tY -= (1.0 - f_reduce) * msgc.v2Rotate.tY;
							}
						}

						// Clamp the hand Y angle.
						SetMinMax(ang2_hand.tY, -PlayerSettings.ang2HandMax.tY, PlayerSettings.ang2HandMax.tY);

						// Convert angles to hand position.
						SetHandPosShoulder(ang2_hand, r_extend);
					}
				} // if (eHandActivity >= ehaACTIVE)
				else if (eHandActivity == ehaINACTIVE)
				{
					// Activate the hand.
					bool b_move = eHandHolding >= ehhHOLDING ? 
						PlayerSettings.bActivateCentreHolding :
						PlayerSettings.bActivateCentreEmpty;

					CAngles2 ang2_hand;

					if (b_move)
						// Place hand in front of head's last position, at centre of valid view area.
						ang2_hand = (PlayerSettings.ang2HandViewMin + PlayerSettings.ang2HandViewMax) * 0.5;
					else
					{
						// Set target to current hand position, and move into view if not already.
						ang2_hand = ang2HandBody(v3_hand) - ang2_head;
						if (!bWithin(ang2_hand.tX, PlayerSettings.ang2HandViewMin.tX, PlayerSettings.ang2HandViewMax.tX) ||
							!bWithin(ang2_hand.tY, PlayerSettings.ang2HandViewMin.tY, PlayerSettings.ang2HandViewMax.tY))
						{
							SetMinMax(ang2_hand.tX, PlayerSettings.ang2HandViewMin.tX, PlayerSettings.ang2HandViewMax.tX);
							SetMinMax(ang2_hand.tY, PlayerSettings.ang2HandViewMin.tY, PlayerSettings.ang2HandViewMax.tY);
							b_move = true;
						}
					}

					if (b_move)
					{
						// Move the hand.
						ang2_hand += ang2_head;
						p3Hand.v3Pos = v3HandBody(ang2_hand, r_extend);
					}

					if (eHandHolding == ehhEMPTY)
					{
						// Only if empty, set rotation to unit rotation.
						bHandRotate = false;
					}

					if (eHandHolding < ehhHOLDING)
						// If truly inactive, protect it before setting it active.
						ProtectHand(false);

					if (PlayerSettings.bHeadFollowActual)
					{
						// Set hand activity to RAISING status.
						eHandActivity = ehaRAISING;
						sRaiseTime = CMessageStep::sStaticTotal + PlayerSettings.sRaiseTime;
					}
					else
					{
						// Set hand to active.
						eHandActivity = ehaACTIVE;
					}
				} // else if (eHandActivity == ehaINACTIVE)

				//
				// Rotate wrist.
				//

				if ((msgc.bPressed(uCMD_SHIFT) || msgc.bPressed(uCMD_CONTROL)) /*&& (msgc.v2Rotate.tX || msgc.v2Rotate.tY)*/)
				{
					if (msgc.bPressed(uCMD_SHIFT) && msgc.bPressed(uCMD_CONTROL))
					{
						// When both pressed, reset to default.
						SetHandRotate();
						if (!bHandRotate)
							// Not holding anything, reset to default orientation.
							// Get current requested rotation from hand's actual position.
							p3Hand.r3Rot = r3Null;
					}

					else if (!bHandRotate)
					{
						// First time, and hand not rotated.
						// Get current requested rotation from hand's actual position.
						p3Hand.r3Rot = pinsHand->r3Rot() / (r3HandRequestSpace() * r3Rot());
						p3Hand.r3Rot.Normalise(true);
					}

					//
					// Apply mouse rotation to hand orientation, in request space.
					// The rotation deltas are always in absolute request space, not local hand space.
					// This allows greater variety of rotation, and more intuitive control.
					//

					// Clip the amount of rotation in any given frame, so that angle limits don't get confused.
					CVector2<> v2_rot
					(
						MinMax(msgc.v2Rotate.tX, -30 * dDEGREES, +30 * dDEGREES),
						MinMax(msgc.v2Rotate.tY, -60 * dDEGREES, +60 * dDEGREES)
					);

					CRotate3<> r3_rot = msgc.bPressed(uCMD_CONTROL) ? 
						// If Control is pressed, X controls roll.
						CRotate3<>(d3YAxis, v2_rot.tX) :
						// Else, X controls yaw, -Y controls pitch.
						CRotate3<>(d3XAxis, -v2_rot.tY) * CRotate3<>(-d3ZAxis, v2_rot.tX);

#if 1
					CRotate3<> r3_new = p3Hand.r3Rot * r3_rot;
					CAngles3 ang3_cmd = Angles3(r3_new * r3HandRequestSpace() / r3HandLimitSpace(v3HandActual()));

					if (
						bWithin(ang3_cmd.tX, PlayerSettings.ang3HandRotMin.tX, PlayerSettings.ang3HandRotMax.tX) &&
						bWithin(ang3_cmd.tY, PlayerSettings.ang3HandRotMin.tY, PlayerSettings.ang3HandRotMax.tY) &&
						bWithin(ang3_cmd.tZ, PlayerSettings.ang3HandRotMin.tZ, PlayerSettings.ang3HandRotMax.tZ)
					)
						p3Hand.r3Rot = r3_new;
#else
					p3Hand.r3Rot *= r3_rot;
					ClipHandRotate();
#endif
					p3Hand.r3Rot.Normalise(true);
					bHandRotate = true;
				}

			} // if (!bWithin(eHandHolding, ehhGRABBING, ehhGRABBED))
		} // if (msgc.bPressed(uCMD_HAND))
		else
		{
			// Let go the hand.
			eHandActivity = ehaINACTIVE;
		}

		//
		// Adjust hand for holding.
		//

		if (bWithin(eHandHolding, ehhHOLDING, ehhDROPPING))
		{
			// Player continues to hold object. Effect holding behaviour.
			bool b_grasp = true;		// Tell the physics system to bend the fingers in a grasping motion.
			const CMagnetPair* pmp_pickup = NMagnetSystem::pmpFindMagnet(pinsHeld, Set(emfHAND_HOLD));
			if (pmp_pickup)
			{
				// Do we have a substitute hand mesh for this pickup magnet?
				if (pmp_pickup->pmagData->u1SubstituteMesh != 0)
					// Yes!  Do not bend fingers.  That's done by the substitute mesh.
					b_grasp = false;
			}

			// Pickup instructs hand to grasp the object (handled by TParent).
			msgpr.subPickup.Set(rt_high_urgency, 1.0, b_grasp);
		} // if (eHandHolding == ehhHOLDING)


		//
		// Rotate head.
		//

		if (bVRHead())
		{
			// VR: the head is wherever the headset points, regardless of hand state.
			ang2_head = ang2VRHead(p3GetPlacement().r3Rot);

			// The body turns to face where you look once the head is more than ~25 degrees off,
			// so the shoulders (and both arms) stay sensibly placed.
			const float fVR_TURN = 0.45f;
			if (!bWithin(ang2_head.tX, -fVR_TURN, fVR_TURN))
			{
				// Looking to the side: turn the body to follow (does not affect the view).
				if (ang2_head.tX < 0)
					ang2_body.tX += Max(ang2_head.tX - -fVR_TURN, -dPI_2 * 0.95);
				else 
					ang2_body.tX += Min(ang2_head.tX -  fVR_TURN, dPI_2 * 0.95);

				CRotate3<> r3_body_new = CRotate3<>(d3ZAxis, CAngle(-ang2_body.tX));
				msgpr.subOrientBody.Set(rt_low_urgency, 1, r3_body_new);
			}

			SetMinMax(ang2_head.tX, -dPI_2 * 0.95, dPI_2 * 0.95);
			SetMinMax(ang2_head.tY, -dPI_2 * 0.95, dPI_2 * 0.95);
		}
		else if (eHandActivity == ehaINACTIVE && 
			eHandHolding != ehhDROPPING)
		{
			// Apply mouse directly to head (kinda like that scene in 1984).
			ang2_head += msgc.v2Rotate;

			if (!bWithin(ang2_head.tX, -PlayerSettings.fAngHeadTurnX, PlayerSettings.fAngHeadTurnX))
			{
				// Head has swung around too much, drag the body, but only by a quarter turn max.
				if (ang2_head.tX < 0)
					ang2_body.tX += Max(ang2_head.tX - -PlayerSettings.fAngHeadTurnX, -dPI_2 * 0.95);
				else 
					ang2_body.tX += Min(ang2_head.tX -  PlayerSettings.fAngHeadTurnX, dPI_2 * 0.95);

				CRotate3<> r3_body_new = CRotate3<>(d3ZAxis, CAngle(-ang2_body.tX));
				msgpr.subOrientBody.Set(rt_low_urgency, 1, r3_body_new);
			}

			// Limit total turn to a quarter rotation, because body will be turned to this orientation
			// if we walk.
			SetMinMax(ang2_head.tX, -dPI_2 * 0.95, dPI_2 * 0.95);
			SetMinMax(ang2_head.tY, -dPI_2 * 0.95, dPI_2 * 0.95);
		}

		// Pass the requested head rotation.  Also, update p3Head here directly for convenience,
		// even though the CMessagePhysicsReq handler does it anyway.
		SetHead(r3FromAngles(ang2_head));

		//
		// Finally, set the hand commands.
		//
		if (eHandActivity > ehaINACTIVE || eHandHolding >= ehhHOLDING || iStowing || iSwinging)
		{
			//
			// Get hand angles in shoulder space, and clamp them.
			//
			CVector3<> v3_hand = p3Hand.v3Pos;

			if (!iStowing && !bThrowing && eHandHolding != ehhGRABBING)
			{
				CVector3<> v3_hand_shoulder = v3_hand - v3Shoulder();
				CAngles2 ang2_hand = Angles2(CDir3<>(v3_hand_shoulder));
			
				if (!bWithin(ang2_hand.tX, -PlayerSettings.ang2HandMax.tX, PlayerSettings.ang2HandMax.tX) ||
					!bWithin(ang2_hand.tY, -PlayerSettings.ang2HandMax.tY, PlayerSettings.ang2HandMax.tY))
				{
					SetMinMax(ang2_hand.tX, -PlayerSettings.ang2HandMax.tX, PlayerSettings.ang2HandMax.tX);
					SetMinMax(ang2_hand.tY, -PlayerSettings.ang2HandMax.tY, PlayerSettings.ang2HandMax.tY);
					v3_hand = v3HandShoulder(ang2_hand, v3_hand_shoulder.tLen());
				}
			}

			msgpr.subUserMoveHand.Set(rt_high_urgency, 1.0, v3_hand);

			if (bHandRotate && !msgpr.subUserOrientHand.rtUrgency)
			{
				// Pass rotation relative to shoulder position.
				// Translate from request (head) space to command (shoulder) space.
				msgpr.subUserOrientHand.Set(rt_low_urgency, 1.0, 
					p3Hand.r3Rot * r3HandRequestSpace());
			}

			// Inform physics we are swinging, so we can add some oomph.
			msgpr.subSwing.Set(rt_high_urgency, 1.0, iSwinging);
		}

		//
		// Move body.
		//

		if (!msgc.v2Move.bIsZero())
		{
			// Walking. Set walk target to current position plus walk vector.
			// Translate walk vector magnitude to speed rating.
			// CODE GEN BUG HERE IN OPTIMIZED BUILD! RESULTS IN UNROTATED v3_walk.
			CRotate3<> r3_head_global = p3Head.r3Rot * p3GetPlacement().r3Rot;
			CVector3<> v3_walk = CVector3<>(msgc.v2Move) * r3_head_global;
			CVector3<> v3_target = p3GetPlacement().v3Pos + v3_walk;
			float	   f_speed = Min(msgc.v2Move.tLen(), 1.0);

			msgpr.subMoveBody.Set(rt_high_urgency, f_speed, v3_target);

			// Also specify desired orientation equal to head facing.
			msgpr.subOrientBody.Set(rt_low_urgency, 1, r3_head_global);
		}

		// VR: Anne's body follows the headset (stick locomotion and walking around the room).
		BodyFollow();

		// Pass crouch command.  'Crouch' really sounds funny after you think the word a lot.
		if (!bWithin(eHandHolding, ehhSEEKING, ehhGRABBING))
			// Turn off auto-crouch whenever leaving grab state.
			bAutoCrouching = false;
		msgpr.subCrouch.Set(rt_high_urgency, 1, bAutoCrouching || msgc.bPressed(uCMD_CROUCH));

		// Send jump command only once per key press.
		msgpr.subJump.dData = false;
		if (msgc.bHit(uCMD_JUMP))
			msgpr.subJump.Set(rt_high_urgency, 1, true);

		if (msgc.bHit(uCMD_REPLAYVO))
        {
		    CMessageAudio	msg
			    (
				    NULL,
				    eamREPLAY_VOICEOVER,		// type of sound
				    NULL,						// sender (NULL if using play function)
				    padAudioDaemon,				// receiver (NULL if using play function)
				    this,						// parent of the effect (can be NULL for stereo)
				    0.0f,						// volume 0dBs
				    0.0f,						// attenuation (only for pseudo/real 3D samples)
				    AU_STEREO,					// spatial type
				    360.0f,						// fustrum angle (real 3D only)
				    -15.0f,						// outside volume (real 3D only)
				    false,						// looped
				    0,							// loop count (pseudo/real 3D only)
				    10000.0f					// distance before sound is stopped (pseudo/real 3D only)
			    );
		    msg.Dispatch();
        }

		// Pass the new physics request directly to our handler (no need to Dispatch()).
		Process(msgpr);
	}

	//******************************************************************************************
	virtual void Process(const CMessageCollision& msgcoll) override
	{
		CTimeBlock tmb(&psCollisionMsgPlayer);

		if (bPhysics && !bReallyDead())
		{
			if (eHandHolding == ehhSEEKING)
			{
				// If player trying to pick up, handle object pickups based on collision with hand.
				// Store the collided-with object in the player, so that next frame, she will pick it up.
				Assert(!pinsHeld);
				CInstance* pins_collide = 0;

				if (pinsHand == msgcoll.pins1)
					pins_collide = msgcoll.pins2;
				else if (pinsHand == msgcoll.pins2)
					pins_collide = msgcoll.pins1;

				if (pins_collide)
				{
					// Ignore any box associated with player or other animal.
					// To do: implement grabbing animals.
					if (bPickupable(pins_collide))
					{
						// Start grabbing this box now.
						eHandHolding = ehhGRABBED;
						pinsHeld = pins_collide;
					}
				}
			}
		}

		//
		// Now we do a little something extra. Detect dino footsteps and body crashes.
		// Footsteps are indicated by the animal instance; we are also interested in
		// bodies (not other parts) hitting the terrain, for death shake, etc.
		//
		CAnimal* pani = ptCast<CAnimal>(msgcoll.pins1);
		if (!pani)
		{
			CBoundaryBox* pbb = ptCast<CBoundaryBox>(msgcoll.pins1);
			if (pbb && pbb->ebbElement == ebbBODY)
			{
				pani = ptCast<CAnimal>(pbb->paniAnimate);
			}
		}

		if (pani && !pani->bReallyDead() && ptCast<CTerrain>(msgcoll.pins2))
		{
			//
			// HACK: Because animal masses are not realistic, collision energy isn't as well.
			// We'll do the shake only for animals above a certain size; we just grab the
			// normalised sound volume, and scale it by the size.
			//

			float f_extent = pani->fGetScale();
			if (f_extent >= fShakeMinSize)
			{
				float f_intensity = msgcoll.fEnergyMaxNormLog() * (f_extent / fShakeMaxSize);

				// Adjust intensity of collision by inverse of distance.
				float f_dist_sqr = (msgcoll.v3Position - v3Pos()).tLenSqr();
				f_intensity *= Min(Sqr(f_extent) / f_dist_sqr, 1.0f);

				// Set camera shake.
				SetMax(angShakeAmplitude, f_intensity * angShakeMax);

				// Jostle the player.
				Jostle(f_intensity * fShakeJostle);
			}
		}

		// Don't do anything if the player is invulnerable.
		if (bInvulnerable)
			return;

		// Don't care about collisions with our own hand!
		if (msgcoll.pins1 == pinsHand || msgcoll.pins2 == pinsHand)
			return;

		// Can't hurt ourselves with what we are holding.
		if (pinsHeld && (msgcoll.pins1 == pinsHeld || msgcoll.pins2 == pinsHeld))
			return;

		// Call base class handling code.
		CAnimate::Process(msgcoll);
	}

	//*****************************************************************************************
	virtual void Process(const CMessagePhysicsReq& msgpr) override
	{
		if (!bPhysics)
			return;

		// Move the head as well.
		if (msgpr.subUserOrientHead.rtUrgency != 0)
		{
			// For head, just change the head orientation.  Do not change the physics model.
			SetHead(msgpr.subUserOrientHead.dData);
		}

		// Let parent handle basic body movement.
		CAnimate::Process(msgpr);
	}

	//*****************************************************************************************
	void MoveWithMagnets(CInstance* pins, const CPlacement3<>& p3_new, CInstance* pins_ignore)
	{
		// First drag along any attached object(s), giving them same position relative to mover that it had.
		static CDArray<CMagnetPair*> dapmp(50);
		dapmp.Reset();
		NMagnetSystem::GetAttachedMagnetsSlaves(NMagnetSystem::pinsFindMaster(pins, pins_ignore), &dapmp);

		for (int i = 0; i < dapmp.uLen; i++)
		{
			const CMagnetPair& pmp = *dapmp[i];
			if (pmp.pinsSlave && pmp.pinsSlave != pins && pmp.pinsSlave != pins_ignore)
			{
				// An attached object. Deduce the relative presence, and adjust.
				CPlacement3<> p3_rel = pmp.pinsSlave->p3GetPlacement() / pins->p3GetPlacement();
				pmp.pinsSlave->Move(p3_rel * p3_new);
			}
		}

		// Finally, move the object.
		pins->Move(p3_new);
	}

	//*****************************************************************************************
	virtual void Process(const CMessageMove& msgmv) override
	{
		CTimeBlock tmb(&psMoveMsgPlayer);

		if (msgmv.etType == CMessageMove::etMOVED)
		{
			// Check for attached objects to drag along.

			if (msgmv.pinsMover == pinsHand && bWithin(eHandHolding, ehhHOLDING, ehhDROPPING))
			{
				if (msgmv.petGetSender() == pphSystem)
					// Physics already takes care of held objects.
					return;

				// Drag held object with hand.
				// Deduce the relative placement, and apply.
				CPlacement3<> p3_rel = pinsHeld->p3GetPlacement() / msgmv.p3Prev;
				MoveWithMagnets(pinsHeld, p3_rel * msgmv.pinsMover->p3GetPlacement(), msgmv.pinsMover);
			}
			else if (msgmv.pinsMover == this)
			{
				// Whether moved by physics or other, must drag along stowed object.
				// Use stored stow position.
				if (apinsStowed[0])
					MoveWithMagnets(apinsStowed[0], ap3StowedObj[0] * p3GetPlacement(), this);
				if (apinsStowed[1])
					MoveWithMagnets(apinsStowed[1], ap3StowedObj[1] * p3GetPlacement(), this);
			}
		}
	}

	//*****************************************************************************************
	virtual char* pcSave(char* pc) const override
	{
		// Save the instance location data.
		pc = CAnimate::pcSave(pc);

		// Save the boolean data.
		pc[0] = bHandRotate;
		pc[1] = (0 != bPhysics);		
		pc[2] = iSubstituteRequest;
		pc[3] = int(eHandActivity);
		pc[4] = int(eHandHolding);
		pc[5] = bThrowing;
		pc[6] = iStowing;
		pc[7] = iStowSlot;

		// Leave some extra.
		pc += 8;

		pc = pcSaveT(pc, bControl);

		// Save the various positional data elements.
		pc = r3Body.pcSave(pc);
		pc = p3Head.pcSave(pc);
		pc = p3Hand.pcSave(pc);
		SaveT(&pc, v2AnglesDraw);
		SaveT(&pc, p3Stowed);
		SaveT(&pc, p3Stowing);

		// Save relative stow placements.
		SaveT(&pc, ap3StowedObj[0]);
		SaveT(&pc, ap3StowedObj[1]);

		// Save the stowed and held instance handle!
		pc = pcSaveInstancePointer(pc, apinsStowed[0]);
		pc = pcSaveInstancePointer(pc, apinsStowed[1]);
		pc = pcSaveInstancePointer(pc, pinsHeld);

		// Other bits of state we may as well save.
		SaveT(&pc, bAutoCrouching);
		SaveT(&pc, v2Throw);
		SaveT(&pc, sRaiseTime  - CMessageStep::sStaticTotal);
		SaveT(&pc, sActionTime - CMessageStep::sStaticTotal);

		return pc;
	}

	//*****************************************************************************************
	virtual const char* pcLoad(const char* pc) override
	{
		pc = CAnimate::pcLoad(pc);

		// Save the boolean data.
		bHandRotate			= pc[0];
		bPhysics			= pc[1];
		iSubstituteRequest	= pc[2];
		eHandActivity		= (EHandActivity) pc[3];
		eHandHolding		= (EHandHolding) pc[4];
		bThrowing			= pc[5];
		pc += 6;

		if (CSaveFile::iCurrentVersion >= 9)
		{
			iStowing  = (int8)*pc++;
			iStowSlot = (int8)*pc++;
		}

		if (CSaveFile::iCurrentVersion >= 18)
		{
			pc = pcLoadT(pc, &bControl);
		}

		// Load the various positional data elements.
		pc = r3Body.pcLoad(pc);
		pc = p3Head.pcLoad(pc);
		pc = p3Hand.pcLoad(pc);

		if (CSaveFile::iCurrentVersion >= 11)
		{
			LoadT(&pc, &v2AnglesDraw);
			if (CSaveFile::iCurrentVersion >= 15)
			{
				LoadT(&pc, &p3Stowed);
				LoadT(&pc, &p3Stowing);
			}
			if (CSaveFile::iCurrentVersion >= 22)
			{
				// Load relative stow placements.
				LoadT(&pc, &ap3StowedObj[0]);
				LoadT(&pc, &ap3StowedObj[1]);
			}
		}

		pc = pcLoadInstancePointer(pc, &apinsStowed[0]);
		if (CSaveFile::iCurrentVersion >= 10)
			pc = pcLoadInstancePointer(pc, &apinsStowed[1]);
		if (CSaveFile::iCurrentVersion < 9)
			// This contained useless hand instance.
			apinsStowed[0] = apinsStowed[1] = 0;

		// Load the held instance handle!
		pc = pcLoadInstancePointer(pc, &pinsHeld);

		// Other bits of state we may as well load.
		LoadT(&pc, &bAutoCrouching);
		LoadT(&pc, &v2Throw);
		LoadT(&pc, &sRaiseTime);
		LoadT(&pc, &sActionTime);

		sRaiseTime  += CMessageStep::sStaticTotal;
		sActionTime += CMessageStep::sStaticTotal;

		// Feel free to start talking.
		sDoneTalking = -10.0f;
		u4AudioID = 0;

		return pc;
	}

	//*****************************************************************************************
	virtual void Cast(CPlayer** ppplay) override
	{
		*ppplay = this;
	}

	//*****************************************************************************************
	//
	virtual const char* strPartType
	(
	) const override
	//
	// Returns a partition type string.
	//
	//**************************
	{
		return "Player";
	}

	//*****************************************************************************************
	virtual void InitializeDataStatic() override
	{
		SetFlagHardwareAble(true);
		CPartition::InitializeDataStatic();
	}
};

//******************************************************************************************
//
// class CPlayerPriv implementation.
//

	//*****************************************************************************************
	CPlayerPriv::CPlayerPriv()
		: rnd(1500)
	{
		// Init hand pointer to zero.
		pinsHand = 0;
		pinsAttached = 0;

		// For now, turn this off, as shadowing system deals very poorly with it.
		//SetFlagShadow(false);

		//
		// This flag also causes the camera to always enable polygon clipping,
		// which will always be done normally anyway, but which adds safety if the camera
		// is off of Anne during debugging, and her hand stretches too far.
		//
		SetFlagNoCacheAlone(true);

		// Make sure physics is off BEFORE we move the player.
		bPhysics = false;


		// Done this way to ensure that the info is unique, and that it will be properly deleted.
		CPhysicsInfoPlayer* pphip = new CPhysicsInfoPlayer(this);
		SetPhysicsInfo(pphip);

		CAIInfo* paii = new CAIInfo(eaiUNKNOWN);

		// Ignore the player.  Chase the boxes!
		paii->setFlags[eaifIGNORE] = true;
		paii->rtDanger = 0.5f;
		paii->SetAIMass(0.0f);
		SetAIInfo(paii);
		
		// Set her default position in the world.
		SetPos(CVector3<>(0.0f, 0.0f, 0.5f));

		// Initialise head placement.
		p3Head = CPlacement3<>(PlayerSettings.v3HeadOffset);

		// Default to vulnerable.
		bInvulnerable = false;

		// Default to controllable.
		bControl = true;

		// Set all states off.
		eHandActivity = ehaINACTIVE;
		eHandHolding = ehhEMPTY;
		pinsHeld = 0;
		bAutoCrouching = false;
		bHandRotate = false;
		iSubstitute = 0;
		iSubstituteRequest = 0;
		apinsStowed[0] = apinsStowed[1] = 0;
		iStowSlot = iStowing = 0;
		iSwinging = 0;
		v2AnglesDraw = CVector2<>(0, 0);
		iScreenFlashTime = -1;

		sRaiseTime = 0;

		sDoneTalking = -10.0f;
		u4AudioID = 0;

		// Set the player name.
		SetInstanceName("Player");

		// Reset the handle, since it's based on name.
		SetUniqueHandle(0);

		// Setup the ouch sounds.  Defaults to none.
		afOuchDamageLevels.uLen = 0;
		assOuchSampleSets.uLen  = 0;	// How we say ouch at each level.
		ssOuchDefault.uLen		= 0;	// What we say when we have nothing better to say.
		angShakeAmplitude		= 0.0f;
		// Clear all bullet samples, but activate them all.
		TSampleSet ss;
		for (int i_count = 0; i_count < iMAX_NUMERICAL_BULLET_COUNT; ++i_count)
		{
			assAmmo << ss;
			assPickup << ss;
		}

		Assert(gpPlayer == 0);
		gpPlayer = this;

		// Register this with desired message types.
		      CMessageStep::RegisterRecipient(this);
		      CMessageMove::RegisterRecipient(this);
		 CMessageCollision::RegisterRecipient(this);
		CMessagePhysicsReq::RegisterRecipient(this);
		   CMessageControl::RegisterRecipient(this);
			CMessageSystem::RegisterRecipient(this);
	}

	//*****************************************************************************************
	CPlayerPriv::~CPlayerPriv()
	{
			CMessageSystem::UnregisterRecipient(this);
		   CMessageControl::UnregisterRecipient(this);
		CMessagePhysicsReq::UnregisterRecipient(this);
		 CMessageCollision::UnregisterRecipient(this);
		      CMessageMove::UnregisterRecipient(this);
		      CMessageStep::UnregisterRecipient(this);

		// VR hand boxes belong to the world (they are deleted with it).
		gVR.apvHandBox[0] = gVR.apvHandBox[1] = nullptr;

		// VR second arm.
		if (pvrlaLeftArm)
		{
			if (pvrlaLeftArm->bInWorld)
				wWorld.Remove(pvrlaLeftArm);
			gVR.pvMirrorInstance = nullptr;
			delete pvrlaLeftArm;
			pvrlaLeftArm = nullptr;
		}

		Assert(gpPlayer == this);
		gpPlayer = 0;
	}

	//*****************************************************************************************
	CVector3<> CPlayerPriv::v3Sight() const
	{
		if (bHandFollowHead())
		{
			// Only if head tracking is enabled (i.e. guns) do we sight on the hold magnet position.
			const CMagnetPair* pmp_hold = NMagnetSystem::pmpFindMagnet(pinsHeld, Set(emfSHOULDER_HOLD));
			if (pmp_hold)
			{
				// The sight position is taken from the hand, then moved onto the hold magnet Y axis.

				// Get the held object target placement.
				CPlacement3<> p3_hand = p3Hand;
				p3_hand.r3Rot *= CRotate3<>(d3YAxis, CDir3<>(p3Hand.v3Pos - p3Head.v3Pos));
				CPlacement3<> p3_held = pinsHeld->p3GetPlacement() / pinsHand->p3GetPlacement() * p3_hand;

				// Compensate for the held object sag, only if X axes line up.
				CRotate3<> r3_held = pinsHeld->r3Rot() / r3Rot();
				if ((d3XAxis * p3_held.r3Rot) * (d3XAxis * r3_held) >= 0.95f)
				{
					CVector3<> v3_y_target = d3YAxis * p3_held.r3Rot;
					v3_y_target.tX = 0.0f;

					CVector3<> v3_y_actual = d3YAxis * r3_held;
					v3_y_actual.tX = 0.0f;

					CRotate3<> r3_sag = CRotate3<>(CDir3<>(v3_y_target), CDir3<>(v3_y_actual));
					p3_held.r3Rot *= r3_sag;
				}

				// Get the hold magnet position for the hand target, in player space.
				CVector3<> v3_hold_mag = pmp_hold->pr3MagMaster.v3Pos * pinsHeld->fGetScale() * p3_held;
				CDir3<> d3_sight = d3YAxis * p3_held.r3Rot;

				CVector3<> v3_hold = v3_hold_mag + 
									d3_sight * (d3_sight * (p3Hand.v3Pos - v3_hold_mag));
				return v3_hold;
			}
		}

		// Otherwise, just sight on the hand.
		return p3Hand.v3Pos;
	}

	//*****************************************************************************************
	TReal CPlayerPriv::rHandDistance() const
	{
		if (bWithin(eHandHolding, ehhHOLDING, ehhDROPPING))
		{
			const CMagnetPair* pmp_hold = NMagnetSystem::pmpFindMagnet(pinsHeld, Set(emfSHOULDER_HOLD));
			if (pmp_hold)
			{
				// Use shoulder hold distance for default.
				TReal r_extend = (pmp_hold->v3Pos() - v3HandToPalmSpecial(pinsHand->p3GetPlacement())).tLen();
				SetMax(r_extend, PlayerSettings.fHandDistMin * 0.5);
				return r_extend;
			}
		}

		return PlayerSettings.fHandDistMin;
	}

	//*****************************************************************************************
	void CPlayerPriv::SetHandPosHead(CAngles2 ang2_hand_head, TReal r_extend)
	{
		//
		// Move hand forward or backward from body to maintain constant shoulder distance.
		// To find the scalar r, we solve the equation
		//
		//		| H * r - S | = d,			H = hand pos, S = shoulder pos, d = desired dist
		//
		//	Squaring both sides, expanding the vector equation to 3 scalar equations,
		//	summing them, and rearranging, we have the quadratic equation
		//
		//		H² r² - 2 (H*S) r + S² - d² = 0
		//

		// Hand pos starts out as unit vector in desired direction.
		CVector3<> v3_hand = d3FromAngles(ang2_hand_head);

		// Shoulder offset relative to head origin, which hand extends from.
		CVector3<> v3_offset = v3Shoulder() - v3HeadOrigin();

		TReal ar_results[2];
		if (iSolveQuadratic
			(
				ar_results, 
				1.0f,									// a
				-2.0f * (v3_hand * v3_offset),			// b
				v3_offset.tLenSqr() - Sqr(r_extend)		// c
			)
		)
		{
			// There is a solution!
			Assert(ar_results[1] > 0.0);
			r_extend = ar_results[1];

			p3Hand.v3Pos = v3HeadOrigin() + v3_hand * p3Head.r3Rot * r_extend;
		}
		else
		{
			// Set position relative to head.
			p3Hand.v3Pos = v3_hand * p3Head;

			// Now, adjust hand distance from shoulder.
			if (r_extend == 0.0)
				r_extend = rHandDistance();
			AdjustHandDist(r_extend);
		}
	}

	//*****************************************************************************************
	void CPlayerPriv::SetHandPosShoulder(CAngles2 ang2_hand_shoulder, TReal r_extend)
	{
		if (r_extend == 0.0)
			r_extend = rHandDistance();

		// Set position relative to shoulder.
		p3Hand.v3Pos = v3Shoulder() + d3FromAngles(ang2_hand_shoulder) * r_extend;
	}

	//*****************************************************************************************
	void CPlayerPriv::AdjustHandDist(TReal r_dist)
	{
		// Adjust hand distance from origin.
		CVector3<> v3_origin = v3Shoulder();
		CDir3<> d3_hand = p3Hand.v3Pos - v3_origin;
		p3Hand.v3Pos = v3_origin + d3_hand * r_dist;
	}

	//*****************************************************************************************
	void CPlayerPriv::SetHead(CRotate3<> r3_head)
	{
		// Remember head-relative hand position.
		CVector3<> v3_hand_head = p3Hand.v3Pos / p3Head;

		p3Head.r3Rot = r3_head;
		if (bVRHead())
			// VR: the real neck moves the headset; don't add the game's neck model on top.
			p3Head.v3Pos = PlayerSettings.v3HeadOffset;
		else
			p3Head.v3Pos = PlayerSettings.v3NeckOffset +
						   (PlayerSettings.v3HeadOffset - PlayerSettings.v3NeckOffset) * r3_head;

		if (!bDead() && eHandActivity == ehaINACTIVE && bHandFollowHead())
		{
			// Remember current distance from shoulder.
			TReal r_dist = (p3Hand.v3Pos - v3Shoulder()).tLen();

			// Maintain head-relative hand position.
			p3Hand.v3Pos = v3_hand_head * p3Head;

			// Maintain original distance from shoulder.
			AdjustHandDist(r_dist);
		}
	}

	//*****************************************************************************************
	float CPlayerPriv::angShakeIntensity() const
	{
		// Just find phase using current global time, and shake frequency.
		float f_sin = sin(CMessageStep::sStaticTotal * d2_PI / sShakePeriod);
		return f_sin * angShakeAmplitude;
	}

	//*****************************************************************************************
	void CPlayerPriv::ClipHandRotate()
	{
		// Clip angles in hand limit space.
		CRotate3<> r3_cmd = p3Hand.r3Rot * r3HandRequestSpace() / r3HandLimitSpace(v3HandActual());
		CAngles3 ang3_cmd = Angles3(r3_cmd);

		// Apply an extra limit to Z rotation when grabbing an object;
		// it looks stupid when she rotates her palm up.
		CAngles3 ang3_clamp
		(
			MinMax(ang3_cmd.tX, PlayerSettings.ang3HandRotMin.tX, PlayerSettings.ang3HandRotMax.tX),
			MinMax(ang3_cmd.tY, PlayerSettings.ang3HandRotMin.tY, PlayerSettings.ang3HandRotMax.tY),
			MinMax(ang3_cmd.tZ, PlayerSettings.ang3HandRotMin.tZ, 
				eHandHolding == ehhGRABBING ? 90 * dDEGREES : PlayerSettings.ang3HandRotMax.tZ)
		);
		if (ang3_clamp != ang3_cmd)
		{
			// We clamped...go all the way back.
			r3_cmd = r3FromAngles(ang3_clamp);
			p3Hand.r3Rot = r3_cmd * r3HandLimitSpace(v3HandActual()) / r3HandRequestSpace();
		}
	}

	//*****************************************************************************************
	void CPlayerPriv::SetHandRotate()
	{
		bHandRotate = false;

		if (bWithin(eHandHolding, ehhHOLDING, ehhDROPPING))
		{
			const CMagnetPair* pmp_hold = NMagnetSystem::pmpFindMagnet(pinsHeld, Set(emfSHOULDER_HOLD));
			if (pmp_hold)
			{
				//
				// pmp_hold->pr3Master specifies hold orientation relative to object.
				// Here we set initial orientation, although player can alter it.
				// We must set this as a hand rotation relative to hand request space.
				// The way to do this is r3_hand_req = r3_hand_hold / r3_req.
				// = r3_hand / r3_object / r3_hold_object / r3_body / r3_req_body.
				//
				p3Hand.r3Rot = pinsHand->r3Rot() / pinsHeld->r3Rot() / pmp_hold->pr3RelMaster().r3Rot;
				ClipHandRotate();
				bHandRotate = true;
			}
		}
	}

	//*****************************************************************************************
	bool CPlayerPriv::bSwingable() const
	{
		// Must be of stowable size and type, and also rather light.
		return iStowable() >= 0 &&
			pinsHeld->pphiGetPhysicsInfo()->fMass(pinsHeld) <= PlayerSettings.fMaxMassSwing;
	}

	//*****************************************************************************************
	int CPlayerPriv::iStowable() const
	{
		if (!bHoldingFreeObj())
			return -1;

		// We want the bounding box of the hand/object set relative to the hand.
		// Fortunately, since the hand is physically active, physics knows this already.
		int i_index = pphSystem->iGetIndex(pinsHand);
		Assert(i_index >= 0);

		CVector3<> v3_min = v3Convert(&Xob[i_index].Extents[0]),
				   v3_max = v3Convert(&Xob[i_index].Extents[3]);

		// Check the stow slots.
		for (int i = 0; i < 2; i++)
		{
			if
			(
				v3_min.tX >= PlayerSettings.av3StowMin[i].tX &&
				v3_min.tY >= PlayerSettings.av3StowMin[i].tY &&
				v3_min.tZ >= PlayerSettings.av3StowMin[i].tZ &&
				v3_max.tX <= PlayerSettings.av3StowMax[i].tX &&
				v3_max.tY <= PlayerSettings.av3StowMax[i].tY &&
				v3_max.tZ <= PlayerSettings.av3StowMax[i].tZ
	 		)
				return i;
		}

		// It's too big.
		return -1;
	}

	//*****************************************************************************************
	void CPlayerPriv::AttemptSubstitute(int i_sub)
	{
		if (iSubstitute == i_sub)
			return;

		// Check our new hand box, and bail if it would intersect any other objects.
		if (papphibHandBoxes[i_sub] && papphibHandBoxes[i_sub] != pinsHand->pphiGetPhysicsInfo())
		{
			// Move new box to match current hand.
			CPresence3<> pr3_cur = pinsHand->pr3GetPresence();
			pr3_cur.v3Pos += 
				(papphibHandBoxes[i_sub]->v3Pivot * pinsHand->fGetScale() - apbbBoundaryBoxes[(int)ebbHAND]->v3GetOffset()) 
				* pr3_cur.r3Rot;

			CWDbQueryPhysicsBoxFast	wqphb(*papphibHandBoxes[i_sub]->pbvGetBoundVol(), pr3_cur);

			for (; wqphb; ++wqphb)
			{
				if (*wqphb!= pinsHand)
					// Found an intersecting box that isn't the hand itself.
					return;
			}

			// Substitute the hand box. Deactivate hand, replace its physics, reactivate.
			pinsHand->PhysicsDeactivate();
			pinsHand->SetPhysicsInfo(const_cast<CPhysicsInfoBox*>(papphibHandBoxes[i_sub]));
			pinsHand->Move(pr3_cur);
			pinsHand->PhysicsActivate();
		}

		// Perform mesh substitution.
		CAnimate::Substitute(i_sub);

		iSubstitute = i_sub;
	}

	//*****************************************************************************************
	void CPlayerPriv::ProtectHand(bool b_ignore)
	{
		//
		// Due to our irresponsible moving and activation of the hand,
		// it may be interpenetrating thangs. If this is so, we first move
		// the hand into the body, a safe haven from penetration. We then raise
		// the hand as normal, since it will ignore body collisions until free.
		//

		// Query objects intersecting the hand.
		CWDbQueryPhysicsBoxFast	wqphb(*pinsHand->pbvBoundingVol(), pinsHand->pr3GetPresence());
		if (eHandHolding == ehhHOLDING)
		{
			Assert(pinsHeld);

			// Add any objects intersecting the held object.
			CWDbQueryPhysicsBoxFast	wqphb2(*pinsHeld->pbvBoundingVol(), pinsHeld->pr3GetPresence());
			wqphb.insert(wqphb.end(), wqphb2.begin(), wqphb2.end());
		}

		for (; wqphb; ++wqphb)
		{
			CInstance* pins = *wqphb;

			// Skip any held or stowed objects.
			if (eHandHolding == ehhHOLDING && pins == pinsHeld)
				continue;
			if (pins == apinsStowed[0] || pins == apinsStowed[1])
				continue;

			if (b_ignore)
			{
				// Skip the hand.
				if (pins == pinsHand)
					continue;

				// Ignore all intersecting objects.
				pphSystem->IgnoreCollisions(pinsHand, pins, true);
			}
			else
			{
				// Skip our own hand, or other parts of us.
				CBoundaryBox* pbb;
				if (pbb = ptCast<CBoundaryBox>(pins))
					if (pbb->paniAnimate == this)
						// It's just one of our boxes.
						continue;

				// Found an intersecting object.
				// Move the hand into the body, by setting its XY to the body's.
				CPlacement3<> p3_hand = pinsHand->p3GetPlacement();
				p3_hand.v3Pos.tX = p3GetPlacement().v3Pos.tX;
				p3_hand.v3Pos.tY = p3GetPlacement().v3Pos.tY;

				pinsHand->Move(p3_hand);
				break;
			}
		}

		if (!b_ignore)
		{
			// Always ignore collisions on the body and foot.
			pphSystem->IgnoreCollisions(pinsHand, apbbBoundaryBoxes[ebbBODY], true);
			pphSystem->IgnoreCollisions(pinsHand, apbbBoundaryBoxes[ebbFOOT], true);
		}
	}

	//*****************************************************************************************
	bool CPlayerPriv::bHandFollowHead() const
	{
		// Follow if holding a non-attached gun.
		return bHoldingFreeObj() && ptCast<CGun>(pinsHeld) && 
			!iStowing && !bThrowing && !iSwinging;
	}


	//*****************************************************************************************
	void CPlayerPriv::CheckWristDrop()
	{
		//
		// Check for twist drop only if not picking up, stowing, throwing, or swinging.
		//
		if 
		(
			!PlayerSettings.bAllowDrop ||
			!bWithin(eHandHolding, ehhHOLDING, ehhDROPPING) || 
			CMessageStep::sStaticTotal < sRaiseTime ||
			iStowing || bThrowing || iSwinging
		)
			return;

		// Get current local placements of hand and shoulder in world.
		CPlacement3<> p3_hand = p3HandActual();

		// Convert from wrist to hand space.
		p3_hand.v3Pos += PlayerSettings.v3WristToPalmSpecial * p3_hand.r3Rot;

		// Check hand angle in command space.
		CRotate3<> r3_cmd = p3_hand.r3Rot / r3HandLimitSpace(p3_hand.v3Pos);
		CAngles3 ang3_cmd = Angles3(r3_cmd);

		for (EAxis ea = eX; ea <= eZ; ea = EAxis(ea+1))
		{
			if (!bWithin(ang3_cmd[ea], 
				PlayerSettings.ang3HandDropMin[ea], PlayerSettings.ang3HandDropMax[ea]))
			{
#if VER_TEST
				// Drop if our wrist is too twwwwwwwisted.
				dout <<"Drop due to twisted " <<"XYZ"[ea] 
					<<": " <<ang3_cmd[ea] / dDEGREES
					<<" [" <<PlayerSettings.ang3HandDropMin[ea]  / dDEGREES
					<<" " <<PlayerSettings.ang3HandDropMax[ea] / dDEGREES
					<<"]\n";

				CAngles3 ang3_req = Angles3(p3Hand.r3Rot * r3HandRequestSpace() / r3HandLimitSpace(v3HandActual()));
				dout <<" Target wrist = " <<ang3_req / dDEGREES <<"\n";
#endif
				Drop();
				return;
			}
		}
	}

	//*****************************************************************************************
	void CPlayerPriv::HandleDrop(const CMessageControl& msgc)
	{
		// Check for forced drop first.
		CheckWristDrop();

		if (eHandHolding == ehhHOLDING)
		{
			// Player is still holding object.
			if (msgc.bHit(uCMD_GRAB))
			{
				// Prepare to drop the object.
				eHandHolding = ehhDROPPING;

				// Start accumulating throw vector.
				v2Throw = CVector2<>(0, 0);
			}
		}

		if (eHandHolding == ehhDROPPING)
		{
			// Player preparing to drop object.
			// Accumulate throw vector while dropping.
			v2Throw += msgc.v2Rotate;
			SetMinMax(v2Throw.tY, -1.0, 1.0);

			// Wait for button to come up.
			if (!msgc.bPressed(uCMD_GRAB))
			{
#if bDROP_THROW
				//
				// Drop the object.
				//

				// Calculate throw vector from current hand position, in head space.
				CDir3<> d3_to_hand = v3HandToPalmNormal(p3HandActual()) / p3Head;
				if (PlayerSettings.bThrowReverse)
					d3_to_hand.tX *= -1;

				if (v2Throw.tY > 0)
				{
					// Rotate throw vector upwards some.
					d3_to_hand *= CRotate3<>(d3XAxis, CAngle(PlayerSettings.angThrowLob));
				}

				// Convert to world space.
				d3_to_hand *= (p3Head.r3Rot * p3GetPlacement().r3Rot);

				TReal r_throw_mag = Abs(v2Throw.tY) * PlayerSettings.fThrowMomentumScale;
				CVector3<> v3_throw = d3_to_hand * r_throw_mag;

				Drop(v3_throw);
#else
				Drop();
#endif
			}
		}
	}

	//*****************************************************************************************
	void CPlayerPriv::HandleThrow(const CMessageControl& msgc)
	{
		if (msgc.bHit(uCMD_THROW))
		{
			if (bHoldingFreeObj())
			{
				if (!bThrowing)
				{
					// Initiate throw sequence.
					bThrowing = true;
					sActionTime = CMessageStep::sStaticTotal + PlayerSettings.sThrowTime;
				}
			}
			else if (bAllowStab)
			{
/*
				// Stab yourself.
				CMessageCollision
				(
					apbbBoundaryBoxes[(int)ebbBODY], 0,
					0, 0,
					0, CRandom::randMain(5000.0, 10000.0), 0,
					pinsHand->v3Pos(),
					0.0f,
					0.0f,
					this
				).Dispatch();
*/
				Jostle(fShakeJostle);
			}
		}

		if (bThrowing)
		{
			// Ignore collisions between both hand and body while throwing, including the release frame.
			pphSystem->IgnoreCollisions(pinsHand, apbbBoundaryBoxes[(int)ebbBODY], true);
			pphSystem->IgnoreCollisions(pinsHand, apbbBoundaryBoxes[(int)ebbFOOT], true);

			if (CMessageStep::sStaticTotal >= sActionTime)
			{
				// Time to throw.
				// Throw the object straight in front of you, up a bit, at max velocity.
				CDir3<> d3_throw(0, 1.0, 0.5);
				CVector3<> v3_throw = (d3_throw * PlayerSettings.fThrowMomentumScale) *
					(p3Head.r3Rot * p3GetPlacement().r3Rot);

				Drop(v3_throw);
				bThrowing = false;
			}
			else
			{
				// Move the hand to the throw position.
				p3Hand = PlayerSettings.p3Throw;
				bHandRotate = true;
			}
		}
	}

	//*****************************************************************************************
	//*****************************************************************************************
	//
	// VR mod: hands, arms and body.
	//
	//*****************************************************************************************

	CPlacement3<> CPlayerPriv::p3VRController(int i_hand) const
	{
		const float* af_q = gVR.aafHandRot[i_hand];
		const float* af_p = gVR.aafHandPos[i_hand];
		return CPlacement3<>
		(
			CRotate3<>(af_q[0], af_q[1], af_q[2], af_q[3], true),
			CVector3<>(af_p[0], af_p[1], af_p[2])
		);
	}

	//*****************************************************************************************
	void CPlayerPriv::IgnoreVR(int i_hand, CInstance* pins, bool b_ignore)
	{
		// Collisions between an object and Anne's body boxes and both VR hand boxes.
		if (!pins)
			return;
		for (int i = 0; i < (int)ebbEND; i++)
			if (apbbBoundaryBoxes[i] && apbbBoundaryBoxes[i] != pins)
				pphSystem->IgnoreCollisions(apbbBoundaryBoxes[i], pins, b_ignore);
		for (int h = 0; h < 2; h++)
			if (avrh[h].pinsBox && avrh[h].pinsBox != pins && avrh[h].bBoxInWorld)
				pphSystem->IgnoreCollisions(avrh[h].pinsBox, pins, b_ignore);
	}

	//*****************************************************************************************
	void CPlayerPriv::ReleaseVR(int i_hand, bool b_throw)
	{
		SVRHand& vh = avrh[i_hand];
		if (!vh.pinsHeld)
			return;

		CInstance* pins = vh.pinsHeld;
		vh.pinsHeld = nullptr;

		if (b_throw)
		{
			// Let go with the hand's velocity (throwing).  Heavy things can't go as fast.
			const float* af_v = gVR.aafHandVel[i_hand];
			CVector3<> v3_vel(af_v[0], af_v[1], af_v[2]);
			float f_mass = pins->pphiGetPhysicsInfo()->fMass(pins);
			float f_max = 20.0f / (1.0f + f_mass / 30.0f);
			if (v3_vel.tLen() > f_max)
				v3_vel *= f_max / v3_vel.tLen();
			pins->pphiGetPhysicsInfoWritable()->ForceVelocity(pins, v3_vel);
		}

		// Keep ignoring collisions with Anne briefly, so a throw doesn't hit her.
		if (vh.pinsReleased && vh.pinsReleased != pins)
			IgnoreVR(i_hand, vh.pinsReleased, false);
		vh.pinsReleased = pins;
		vh.sReleaseTime = CMessageStep::sStaticTotal;
		CMessagePickUp(pins, epuDROP).Dispatch();
	}

	//*****************************************************************************************
	void CPlayerPriv::GrabVR(int i_hand, CInstance* pins, const CPlacement3<>& p3_hand)
	{
		SVRHand& vh = avrh[i_hand];

		// Only one hand holds an object.
		if (avrh[1 - i_hand].pinsHeld == pins)
			ReleaseVR(1 - i_hand, false);

		// Taking it out of a holster?
		for (int i = 0; i < 2; i++)
			if (apinsStowed[i] == pins)
			{
				apinsStowed[i] = 0;
				NMagnetSystem::RemoveMagnetPair(this, pins);
			}

		// Guns (anything with a hold magnet) go into the hand the right way round.
		const CMagnetPair* pmp_pickup = NMagnetSystem::pmpFindMagnet(pins, Set(emfHAND_HOLD));
		if (pmp_pickup)
			vh.p3Grab = pins->p3GetPlacement() * ~p3SpecialPickup(pmp_pickup, p3_hand);
		else
			vh.p3Grab = pins->p3GetPlacement() * ~p3_hand;

		vh.pinsHeld = pins;
		IgnoreVR(i_hand, pins, true);
		for (int h = 0; h < 2; h++)
			if (avrh[h].pinsReleased == pins)
				avrh[h].pinsReleased = nullptr;

		gVR.aiPickupCount[i_hand]++;
		if (gVR.pfnLog)
			gVR.pfnLog("%s hand picked up %s.", i_hand ? "Left" : "Right", pins->strGetInstanceName());
		pins->PickedUp();
		CMessagePickUp(pins, epuPICKUP).Dispatch();
	}

	//*****************************************************************************************
	bool CPlayerPriv::bVRHolster(int i_hand, int i_slot)
	{
		// Holster: stow what the hand holds, or draw what's there.
		if (i_slot < 0 || i_slot > 1)
			return false;

		SVRHand& vh = avrh[i_hand];
		if (vh.pinsHeld)
		{
			if (apinsStowed[i_slot] || vh.pinsHeld->pphiGetPhysicsInfo()->fMass(vh.pinsHeld) > PlayerSettings.fMaxMassSwing * 2)
				return false;

			CInstance* pins = vh.pinsHeld;
			ReleaseVR(i_hand, false);
			vh.pinsReleased = nullptr;	// Stays non-colliding while holstered.

			apinsStowed[i_slot] = pins;
			NMagnetSystem::AddMagnetPair(this, pins, CMagnet::pmagFindShared(CMagnet(Set(emfINTANGIBLE))));
			ap3StowedObj[i_slot] = pins->p3GetPlacement() / p3GetPlacement();
			CMessagePickUp(pins, epuSTOW).Dispatch();
			return true;
		}

		if (!apinsStowed[i_slot])
			return false;

		CInstance* pins = apinsStowed[i_slot];
		CPlacement3<> p3_hand = p3VRController(i_hand);
		GrabVR(i_hand, pins, p3_hand);
		// Put it in the hand right away.
		pins->Move(vh.p3Grab * p3_hand);
		CMessagePickUp(pins, epuRETRIEVE).Dispatch();
		return true;
	}

	//*****************************************************************************************
	void CPlayerPriv::HandleVRHand(int i_hand)
	{
		SVRHand& vh = avrh[i_hand];

		// Restore collisions with a recently released object.
		if (vh.pinsReleased && CMessageStep::sStaticTotal - vh.sReleaseTime > 0.6)
		{
			IgnoreVR(i_hand, vh.pinsReleased, false);
			vh.pinsReleased = nullptr;
		}

		bool b_enabled = bVRHands() && gVR.abHandValid[i_hand] && !bDead() && (i_hand == 0 || gVR.bLeftArm);
		bool b_grip = b_enabled && gVR.abGrip[i_hand];
		bool b_grip_down = b_grip && !vh.bPrevGrip;
		bool b_grip_up = !b_grip && vh.bPrevGrip;
		bool b_trigger = b_enabled && gVR.abTrigger[i_hand];
		bool b_trigger_down = b_trigger && !vh.bPrevTrigger;
		vh.bPrevGrip = b_grip;
		vh.bPrevTrigger = b_trigger;

		int i_holster = gVR.aiHolsterRequest[i_hand];
		gVR.aiHolsterRequest[i_hand] = -1;

		if (!b_enabled)
		{
			ReleaseVR(i_hand, false);
			// Take the hand box out of the world so it isn't an invisible obstacle.
			if (vh.pinsBox && vh.bBoxInWorld)
			{
				vh.pinsBox->PhysicsDeactivate();
				wWorld.Remove(vh.pinsBox);
				vh.bBoxInWorld = false;
			}
			return;
		}

		CPlacement3<> p3_ctrl = p3VRController(i_hand);
		const float* af_v = gVR.aafHandVel[i_hand];
		CVector3<> v3_hand_vel(af_v[0], af_v[1], af_v[2]);

		//
		// The physical hand: a small box that chases the controller and pushes things.
		//
		if (!vh.pinsBox)
		{
			CPhysicsInfoBox phib
			(
				CBoundVolBox(0.09f, 0.16f, 0.06f),	// Width, length, thickness (m).
				SPhysicsData(true, true),
				3.0f								// Mass (kg).
			);
			vh.pinsBox = new CInstance(CInstance::SInit
			(
				CPresence3<>(p3_ctrl.r3Rot, 1.0f, p3_ctrl.v3Pos), rptr0,
				const_cast<CPhysicsInfoBox*>(CPhysicsInfoBox::pphibFindShared(phib)), 0,
				i_hand ? "VRHandBoxL" : "VRHandBoxR"
			));
			vh.bBoxInWorld = false;
			gVR.apvHandBox[i_hand] = vh.pinsBox;
		}
		if (!vh.bBoxInWorld)
		{
			vh.pinsBox->Move(p3_ctrl);
			wWorld.Add(vh.pinsBox);
			vh.bBoxInWorld = true;
			for (int i = 0; i < (int)ebbEND; i++)
				if (apbbBoundaryBoxes[i])
					pphSystem->IgnoreCollisions(apbbBoundaryBoxes[i], vh.pinsBox, true);
			if (avrh[1 - i_hand].pinsBox && avrh[1 - i_hand].bBoxInWorld)
				pphSystem->IgnoreCollisions(avrh[1 - i_hand].pinsBox, vh.pinsBox, true);
			vh.pinsIgnoreOther = nullptr;
		}

		// Don't fight the other hand's object.
		CInstance* pins_other = avrh[1 - i_hand].pinsHeld;
		if (pins_other != vh.pinsIgnoreOther)
		{
			if (vh.pinsIgnoreOther)
				pphSystem->IgnoreCollisions(vh.pinsBox, vh.pinsIgnoreOther, false);
			if (pins_other)
				pphSystem->IgnoreCollisions(vh.pinsBox, pins_other, true);
			vh.pinsIgnoreOther = pins_other;
		}

		{
			CPhysicsInfo* pphi = vh.pinsBox->pphiGetPhysicsInfoWritable();
			CVector3<> v3_cur = vh.pinsBox->v3Pos();
			CVector3<> v3_delta = p3_ctrl.v3Pos - v3_cur;

			if (v3_delta.tLen() > 0.6f)
			{
				// Lost (stuck behind something): snap back to the controller.
				vh.pinsBox->Move(p3_ctrl);
				pphi->ForceVelocity(vh.pinsBox, CVector3<>(0, 0, 0));
			}
			else
			{
				CVector3<> v3_vel = v3_delta / 0.03f;
				if (v3_vel.tLen() > 20.0f)
					v3_vel *= 20.0f / v3_vel.tLen();
				// Rotation follows the controller exactly; Move resyncs physics, then set velocity.
				vh.pinsBox->Move(CPlacement3<>(p3_ctrl.r3Rot, v3_cur));
				pphi->ForceVelocity(vh.pinsBox, v3_vel);
			}
		}

		//
		// Holding.
		//
		if (vh.pinsHeld)
		{
			if (b_grip_up)
			{
				if (!bVRHolster(i_hand, i_holster))
					ReleaseVR(i_hand, true);
				return;
			}

			// Use / fire.
			if (b_trigger)
			{
				int i_shots = gVR.iShotCount;
				vh.pinsHeld->bUse(!b_trigger_down);
				CMessageUse msguse(vh.pinsHeld);
				msguse.Dispatch();
				if (gVR.iShotCount != i_shots)
					gVR.aiShotCount[i_hand]++;
			}

			if (!vh.pinsHeld)
				return;

			// The object sits exactly in the hand (no lag); it moves with the hand's velocity so
			// it still knocks things over and hits dinosaurs.
			CPhysicsInfo* pphi = vh.pinsHeld->pphiGetPhysicsInfoWritable();
			CPlacement3<> p3_target = vh.p3Grab * p3_ctrl;
			float f_mass = pphi->fMass(vh.pinsHeld);
			const float fTOO_HEAVY = 150.0f;	// Above this, things are dragged, not lifted.

			if (f_mass > fTOO_HEAVY)
			{
				CVector3<> v3_delta = p3_target.v3Pos - vh.pinsHeld->v3Pos();
				if (v3_delta.tLen() > 1.0f)
				{
					ReleaseVR(i_hand, false);
					return;
				}
				CVector3<> v3_vel = v3_delta / 0.15f;
				CVector3<> v3_now = pphi->p3GetVelocity(vh.pinsHeld).v3Pos;
				v3_vel.tZ = Min(v3_now.tZ, 0.0f);
				pphi->ForceVelocity(vh.pinsHeld, v3_vel);
			}
			else
			{
				vh.pinsHeld->Move(p3_target);
				pphi->ForceVelocity(vh.pinsHeld, v3_hand_vel);
			}
			return;
		}

		//
		// Empty hand.
		//
		if (!b_grip_down)
			return;

		// Holster draw.
		if (i_holster >= 0 && bVRHolster(i_hand, i_holster))
			return;

		// Steadying the other hand's gun instead of grabbing.
		if (gVR.iTwoHand == (i_hand == 1 ? 1 : 2))
			return;

		CInstance* pins_found = nullptr;

		// Look along the fingers, below the palm, to the palm side, and in between.
		CVector3<> av3_dirs[4] =
		{
			CVector3<>(0, 1, 0), CVector3<>(0, 0, -1), CVector3<>(0, 0.7f, -0.7f),
			CVector3<>(i_hand ? 1.0f : -1.0f, 0, 0)
		};
		for (int i_dir = 0; i_dir < 4 && !pins_found; i_dir++)
		{
			CVector3<> v3_dir = av3_dirs[i_dir] * p3_ctrl.r3Rot;
			CPlacement3<> p3_cast(CRotate3<>(d3YAxis, CDir3<>(v3_dir)), p3_ctrl.v3Pos - v3_dir * 0.1f);
			CRayCast rc(p3_cast, 0.22f, 0.42f);

			SObjectLoc* pobl;
			while (pobl = rc.poblNextIntersection())
			{
				CInstance* pins = pobl->pinsObject;
				if (pins == pinsHand || pins == this || pins == avrh[0].pinsBox || pins == avrh[1].pinsBox ||
					ptCast<CBoundaryBox>(pins))
					continue;
				if (bPickupable(pins))
				{
					pins = NMagnetSystem::pinsFindMaster(pins);
					// The other hand's object stays in the other hand.
					if (pins != pins_other)
						pins_found = pins;
				}
				break;
			}
		}

		if (pins_found)
			GrabVR(i_hand, pins_found, p3_ctrl);
	}

	//*****************************************************************************************
	void CPlayerPriv::BodyFollow()
	{
		// Anne's body is moved to stay under the headset, at the speed the player moves.  The
		// body physics still deals with slopes, stairs, jumping and collisions; the VR mod pulls
		// the view back if the body can't follow (walls).
		if (!gVR.bActive || !gVR.bBodyTargetValid || bDead())
			return;

		CVector3<> v3_body = p3GetPlacement().v3Pos;

		// Track how fast she has recently been falling (decays over about half a second).
		{
			static float f_prev_z = 0;
			static bool  b_prev = false;
			float f_dt = (float)CMessageStep::sStaticStep;
			float f_down = (b_prev && f_dt > 0.0001f) ? (f_prev_z - v3_body.tZ) / f_dt : 0.0f;
			if (f_down > 30.0f)
				f_down = 0;		// Teleported.
			gVR.fFallSpeed = Max(f_down, gVR.fFallSpeed - 12.0f * f_dt);
			if (gVR.fFallSpeed < 0)
				gVR.fFallSpeed = 0;
			f_prev_z = v3_body.tZ;
			b_prev = true;
		}
		CVector3<> v3_to(gVR.afBodyTarget[0] - v3_body.tX, gVR.afBodyTarget[1] - v3_body.tY, 0);
		float f_dist = v3_to.tLen();

		// Within a few centimetres: no walk command, so her own braking holds her still.
		if (f_dist > 0.04f)
		{
			// Walk with her own legs (the walk controller keeps her balanced): at the player's
			// own speed, plus a gentle pull to close the gap.
			CVector3<> v3_vel = CVector3<>(gVR.afBodyVel[0], gVR.afBodyVel[1], 0) + v3_to / 0.3f;
			if (v3_vel.tLen() > 3.7f)
				v3_vel *= 3.7f / v3_vel.tLen();
			bVRSetSkeletonWalkVelocity(this, v3_vel.tX, v3_vel.tY);
		}
	}

	//*****************************************************************************************
	void CPlayerPriv::PoseVRArm
	(
		CSkeletonRenderInfo& sri, const CTransform3<>* atf3, int i_transforms,
		rptr_const<CBioMesh> pbm, bool b_mirror, CPlacement3<> p3_target, float f_curl
	)
	{
		//
		// Poses Anne's arm joints (10 shoulder, 11 elbow, 12 wrist, 13 fingers) so the hand is at
		// p3_target, starting from the reference pose atf3 (the physics arm, at rest).  Works in
		// Anne's object space.  A mirrored arm is solved as the right arm reaching for the mirror
		// image of the target, then everything is mirrored (x -> -x).
		//
		CPresence3<> pr3 = pr3GetPresence();
		TReal r_inv_scale = 1.0f / pr3.rScale;
		auto v3Obj = [&](const CVector3<>& v3_world) -> CVector3<>
		{
			return ((v3_world - pr3.v3Pos) / pr3.r3Rot) * r_inv_scale;
		};

		CVector3<> v3_s = atf3[10].v3Pos, v3_e = atf3[11].v3Pos, v3_w = atf3[12].v3Pos, v3_f = atf3[13].v3Pos;
		TReal r_upper = (v3_e - v3_s).tLen(), r_fore = (v3_w - v3_e).tLen();
		if (r_upper < 0.01f || r_fore < 0.01f)
			return;

		// The reference hand frame (the physics hand box).
		CVector3<> v3_hand_r = v3Obj(pinsHand->v3Pos());
		CRotate3<> r3_hand_r = pinsHand->r3Rot() / pr3.r3Rot;

		CVector3<> v3_t = v3Obj(p3_target.v3Pos);
		CRotate3<> r3_t = p3_target.r3Rot / pr3.r3Rot;
		if (b_mirror)
		{
			v3_t.tX = -v3_t.tX;
			r3_t = CRotate3<>(r3_t.tC, r3_t.v3S.tX, -r3_t.v3S.tY, -r3_t.v3S.tZ, true);
		}

		// Wrist and finger base relative to the hand box, carried over to the target.
		CRotate3<> r3_delta = ~r3_hand_r * r3_t;
		// The hand is drawn smaller than Anne's original (oversized) hand (ini: HandScale).
		TReal r_hs = gVR.fHandScale;
		CVector3<> v3_w_new = v3_t + ((v3_w - v3_hand_r) / r3_hand_r) * r_hs * r3_t;
		CVector3<> v3_f_new = v3_t + ((v3_f - v3_hand_r) / r3_hand_r) * r_hs * r3_t;

		// Two-bone IK for the elbow.
		CVector3<> v3_d = v3_w_new - v3_s;
		TReal r_d = v3_d.tLen();
		TReal r_max = (r_upper + r_fore) * 0.995f;
		if (r_d > r_max)
		{
			CVector3<> v3_shift = v3_d * (r_max / r_d) - v3_d;
			v3_w_new += v3_shift;
			v3_f_new += v3_shift;
			v3_d = v3_w_new - v3_s;
			r_d = r_max;
		}
		if (r_d < 0.02f)
			return;
		CVector3<> v3_dn = v3_d / r_d;
		TReal r_a = (r_upper * r_upper - r_fore * r_fore + r_d * r_d) / (2 * r_d);
		TReal r_h = sqrt(Max(r_upper * r_upper - r_a * r_a, 0.0f));
		CVector3<> v3_pole(0.5f, -0.3f, -1.0f);		// Elbow out, back and down.
		CVector3<> v3_perp = v3_pole - v3_dn * (v3_pole * v3_dn);
		if (v3_perp.tLen() < 1e-4f)
			v3_perp = CVector3<>(1, 0, 0);
		v3_perp.Normalise();
		CVector3<> v3_e_new = v3_s + v3_dn * r_a + v3_perp * r_h;

		// Bone rotations relative to the reference pose.
		CMatrix3<> mx3_upper(CRotate3<>(CDir3<>(v3_e - v3_s), CDir3<>(v3_e_new - v3_s)));
		CMatrix3<> mx3_fore (CRotate3<>(CDir3<>(v3_w - v3_e), CDir3<>(v3_w_new - v3_e_new)));
		CMatrix3<> mx3_hand (r3_delta);

		// Finger curl (the physics hand is at rest, curl 0).
		CVector3<> v3_palm = CVector3<>(0, 0, -1) * r3_t;
		CVector3<> v3_axis = (v3_f_new - v3_w_new) ^ v3_palm;
		CMatrix3<> mx3_curl;
		if (v3_axis.tLen() > 1e-4f)
		{
			v3_axis.Normalise();
			mx3_curl = CMatrix3<>(CRotate3<>(CDir3<>(v3_axis), CAngle(f_curl * 1.3f * gVR.fCurlSign)));
		}

		if (b_mirror)
		{
			// Everything except the arm collapses to a speck at the shoulder.
			CMatrix3<> mx3_tiny;
			mx3_tiny.v3X = CVector3<>(0.001f, 0, 0);
			mx3_tiny.v3Y = CVector3<>(0, 0.001f, 0);
			mx3_tiny.v3Z = CVector3<>(0, 0, 0.001f);
			CTransform3<> tf3_hidden(mx3_tiny, v3_s);
			for (int j = 0; j < pbm->iJoints; j++)
				sri.patf3JointTransforms[j] = tf3_hidden;
		}

		sri.patf3JointTransforms[10] = CTransform3<>(atf3[10].mx3Mat * mx3_upper, v3_s);
		sri.patf3JointTransforms[11] = CTransform3<>(atf3[11].mx3Mat * mx3_fore,  v3_e_new);
		CMatrix3<> mx3_hscale;
		mx3_hscale.v3X = CVector3<>(r_hs, 0, 0);
		mx3_hscale.v3Y = CVector3<>(0, r_hs, 0);
		mx3_hscale.v3Z = CVector3<>(0, 0, r_hs);
		sri.patf3JointTransforms[12] = CTransform3<>(mx3_hscale * atf3[12].mx3Mat * mx3_hand,  v3_w_new);
		sri.patf3JointTransforms[13] = CTransform3<>(mx3_hscale * atf3[13].mx3Mat * mx3_hand * mx3_curl, v3_f_new);

		if (b_mirror)
		{
			for (int j = 0; j < pbm->iJoints; j++)
			{
				CTransform3<>& tf3 = sri.patf3JointTransforms[j];
				tf3.mx3Mat.v3X.tX = -tf3.mx3Mat.v3X.tX;
				tf3.mx3Mat.v3Y.tX = -tf3.mx3Mat.v3Y.tX;
				tf3.mx3Mat.v3Z.tX = -tf3.mx3Mat.v3Z.tX;
				tf3.v3Pos.tX = -tf3.v3Pos.tX;
			}
		}

		// Double joints: weighted blends of two joints (same as the physics skeleton update).
		for (int j = 0; j < (int)pbm->afPositionRatios.uLen && j + pbm->iJoints < i_transforms; ++j)
		{
			CTransform3<> tf3_primary   = sri.patf3JointTransforms[pbm->aiDoubleJointsA[j]];
			CTransform3<> tf3_secondary = sri.patf3JointTransforms[pbm->aiDoubleJointsB[j]];
			CTransform3<> tf3;

			float f_primary = pbm->afPositionRatios[j];
			tf3.v3Pos = tf3_primary.v3Pos * f_primary + tf3_secondary.v3Pos * (1 - f_primary);

			f_primary = pbm->afRotationRatios[j];
			tf3_primary   *= CScaleI3<>(f_primary);
			tf3_secondary *= CScaleI3<>(1 - f_primary);
			tf3.mx3Mat.v3X = tf3_primary.mx3Mat.v3X + tf3_secondary.mx3Mat.v3X;
			tf3.mx3Mat.v3Y = tf3_primary.mx3Mat.v3Y + tf3_secondary.mx3Mat.v3Y;
			tf3.mx3Mat.v3Z = tf3_primary.mx3Mat.v3Z + tf3_secondary.mx3Mat.v3Z;
			sri.patf3JointTransforms[j + pbm->iJoints] = tf3;
		}
	}

	//*****************************************************************************************
	void CPlayerPriv::VRShowLeftArm(bool b_show)
	{
		// (Despite the name, this poses both arms for the VR view.)
		if (!b_show)
		{
			if (bVRBodyShifted)
			{
				SetPresence(pr3VRRealPresence);
				bVRBodyShifted = false;
			}
			if (pvrlaLeftArm && pvrlaLeftArm->bInWorld)
			{
				wWorld.Remove(pvrlaLeftArm);
				pvrlaLeftArm->bInWorld = false;
			}
			return;
		}

		static int i_last_reason = -1;
		auto log_once = [&](int i_reason, const char* str)
		{
			if (i_reason != i_last_reason && gVR.pfnLog)
				gVR.pfnLog("Arms: %s", str);
			i_last_reason = i_reason;
		};

		if (!bVRHands() || bDead())
		{
			log_once(1, "VR hands off or player dead.");
			return;
		}

		rptr<CRenderType> prdt = prdtGetRenderInfoWritable();
		rptr_const<CBioMesh> pbm = prdt ? prdt->rpbmCast() : rptr_const<CBioMesh>();
		if (!pbm)
		{
			log_once(2, "Anne's mesh is not a skinned mesh.");
			return;
		}
		if (sriBones.patf3JointTransforms.size() < 14 || pbm->iJoints < 14)
		{
			if (gVR.pfnLog && i_last_reason != 3)
				gVR.pfnLog("Arms: skeleton too small (%d joints, %d transforms).", pbm->iJoints, (int)sriBones.patf3JointTransforms.size());
			i_last_reason = 3;
			return;
		}

		// Fresh joints from the physics skeleton (the reference pose).
		const CPhysicsInfoSkeleton* pphis = pphiGetPhysicsInfo()->ppisCast();
		if (pphis)
		{
			bSkeletonUpdated = false;
			pphis->UpdateSkeleton(this);
		}
		int i_transforms = (int)sriBones.patf3JointTransforms.size();
		static std::vector<CTransform3<>> atf3_ref;
		atf3_ref.resize(i_transforms);
		for (int j = 0; j < i_transforms; j++)
			atf3_ref[j] = sriBones.patf3JointTransforms[j];

		// Draw the body directly under the headset, wherever the physics body has got to (it
		// trails a little when you move).  Put back after the view is drawn.
		if (gVR.bBodyTargetValid && !bVRBodyShifted)
		{
			pr3VRRealPresence = pr3GetPresence();
			CVector3<> v3_shift(gVR.afBodyTarget[0] - pr3VRRealPresence.v3Pos.tX,
			                    gVR.afBodyTarget[1] - pr3VRRealPresence.v3Pos.tY, 0);
			if (v3_shift.tLen() < 1.0f)
			{
				CPresence3<> pr3_shifted = pr3VRRealPresence;
				pr3_shifted.v3Pos += v3_shift;
				SetPresence(pr3_shifted);
				bVRBodyShifted = true;
			}
		}

		// The stub of Anne's own left arm is hidden while the VR left arm is shown.
		if (gVR.bLeftArm && gVR.abHandValid[1] && !gVR.bLeftArmTest)
		{
			float f_right_x = atf3_ref[10].v3Pos.tX;
			for (int j = 14; j < pbm->iJoints && j < i_transforms; j++)
			{
				float f_x = atf3_ref[j].v3Pos.tX;
				if (f_x * f_right_x < 0 && fabs(f_x) > 0.5f * fabs(f_right_x))
				{
					CMatrix3<> mx3_tiny;
					mx3_tiny.v3X = CVector3<>(0.001f, 0, 0);
					mx3_tiny.v3Y = CVector3<>(0, 0.001f, 0);
					mx3_tiny.v3Z = CVector3<>(0, 0, 0.001f);
					CVector3<> v3_in = atf3_ref[j].v3Pos;
					v3_in.tX *= 0.5f;		// Tucked inside the chest.
					sriBones.patf3JointTransforms[j] = CTransform3<>(mx3_tiny, v3_in);
					bSkeletonUpdated = true;
				}
			}
		}

		// Hand targets: the controllers, or under the gun grip when steadying a pistol.
		CPlacement3<> ap3_target[2] = { p3VRController(0), p3VRController(1) };
		float af_curl[2];
		for (int h = 0; h < 2; h++)
			af_curl[h] = avrh[h].pinsHeld ? 0.75f : gVR.afCurl[h];

		if (gVR.iTwoHand)
		{
			int i_gun = gVR.iTwoHand == 1 ? 0 : 1;
			int i_support = 1 - i_gun;
			af_curl[i_support] = 1.0f;
			if (gVR.bTwoHandPistol && avrh[i_gun].pinsHeld)
			{
				CVector3<> v3_gun_up = CVector3<>(0, 0, 1) * avrh[i_gun].pinsHeld->r3Rot();
				ap3_target[i_support].v3Pos = ap3_target[i_gun].v3Pos - v3_gun_up * 0.075f;
				ap3_target[i_support].r3Rot = ap3_target[i_gun].r3Rot;
			}
		}

		//
		// Left arm: a mirrored copy of Anne's mesh.
		//
		if (gVR.bLeftArm && gVR.abHandValid[1])
		{
			if (!pvrlaLeftArm || prdtLeftArmMesh != prdt)
			{
				delete pvrlaLeftArm;
				pvrlaLeftArm = new CVRLeftArm(CInstance::SInit(pr3GetPresence(), prdt, 0, 0, "VRLeftArm"));
				prdtLeftArmMesh = prdt;
				gVR.pvMirrorInstance = pvrlaLeftArm;
			}

			CSkeletonRenderInfo& sri = pvrlaLeftArm->sriBones;
			if ((int)sri.patf3JointTransforms.size() == i_transforms)
			{
				for (int j = 0; j < i_transforms; j++)
					sri.patf3JointTransforms[j] = atf3_ref[j];
				if (gVR.bLeftArmTest)
				{
					// Debug: an unmirrored copy of the right arm, 25 cm to the left of the right hand.
					CPlacement3<> p3_test = ap3_target[0];
					p3_test.v3Pos += CVector3<>(-0.25f, 0, 0) * pr3GetPresence().r3Rot;
					gVR.pvMirrorInstance = nullptr;
					PoseVRArm(sri, &atf3_ref[0], i_transforms, pbm, false, p3_test, af_curl[0]);
				}
				else
				{
					gVR.pvMirrorInstance = pvrlaLeftArm;
					PoseVRArm(sri, &atf3_ref[0], i_transforms, pbm, true, ap3_target[1], af_curl[1]);
				}
				sri.bvbVolume = CBoundVolBox(2, 2, 2);
				pvrlaLeftArm->bSkeletonUpdated = true;

				if (!pvrlaLeftArm->bInWorld)
				{
					wWorld.Add(pvrlaLeftArm, true);
					pvrlaLeftArm->bInWorld = true;
				}
				pvrlaLeftArm->Move(pr3GetPresence());
			}
			else
				log_once(4, "left arm joint count mismatch.");
		}

		//
		// Right arm: Anne's own mesh, re-posed (left as updated so rendering doesn't redo it).
		//
		if (gVR.abHandValid[0])
		{
			PoseVRArm(sriBones, &atf3_ref[0], i_transforms, pbm, false, ap3_target[0], af_curl[0]);
			bSkeletonUpdated = true;
		}

		if (gVR.pfnLog && i_last_reason != 0)
			gVR.pfnLog("Arms: shown (%d joints, %d transforms, left arm %s).", pbm->iJoints, i_transforms,
			           pvrlaLeftArm && pvrlaLeftArm->bInWorld ? "in world" : "not shown");
		i_last_reason = 0;

		// Every few seconds: where the second arm's hand is drawn versus the left controller.
		static TSec s_last_report = -100;
		if (gVR.pfnLog && CMessageStep::sStaticTotal - s_last_report > 5.0)
		{
			s_last_report = CMessageStep::sStaticTotal;
			if (pvrlaLeftArm && pvrlaLeftArm->bInWorld && (int)pvrlaLeftArm->sriBones.patf3JointTransforms.size() > 12)
			{
				CVector3<> v3_wrist = pvrlaLeftArm->sriBones.patf3JointTransforms[12].v3Pos * pvrlaLeftArm->pr3GetPresence();
				CVector3<> v3_rwrist = sriBones.patf3JointTransforms[12].v3Pos * pr3GetPresence();
				gVR.pfnLog("Arms: left arm polygons %d seen, %d facing the eye.", gVR.iMirrorPolys, gVR.iMirrorPolysFront);
				gVR.iMirrorPolys = gVR.iMirrorPolysFront = 0;
				gVR.pfnLog("Arms: left wrist drawn at (%.2f %.2f %.2f), left controller (%.2f %.2f %.2f), right wrist (%.2f %.2f %.2f), right controller (%.2f %.2f %.2f).",
					v3_wrist.tX, v3_wrist.tY, v3_wrist.tZ, ap3_target[1].v3Pos.tX, ap3_target[1].v3Pos.tY, ap3_target[1].v3Pos.tZ,
					v3_rwrist.tX, v3_rwrist.tY, v3_rwrist.tZ, ap3_target[0].v3Pos.tX, ap3_target[0].v3Pos.tY, ap3_target[0].v3Pos.tZ);
			}
			else
				gVR.pfnLog("Arms: left arm not drawn (enabled %d, left controller %s, instance %s).",
					(int)gVR.bLeftArm, gVR.abHandValid[1] ? "tracked" : "NOT tracked", pvrlaLeftArm ? "exists" : "none");
		}
	}

	//*****************************************************************************************
	void CPlayerPriv::HandleForceGrab(int i_hand)
	{
		SVRHand& vh = avrh[i_hand];
		CInstance*& pins_target = vh.pinsForceTarget;
		TSec& s_pull_time = vh.sForcePullTime;

		bool b_empty = !vh.pinsHeld && vh.pinsBox && vh.bBoxInWorld;
		if (!bVRHands() || !b_empty)
		{
			gVR.abForceTarget[i_hand] = false;
			gVR.abForcePull[i_hand]   = false;
			if (!b_empty)
				pins_target = nullptr;
			return;
		}

		const float fFORCE_RANGE = 6.0f;

		if (gVR.abForceAim[i_hand])
		{
			// Find the first pickupable object along the pointing ray.
			const float* af_p = gVR.aafAimPosWorld[i_hand];
			const float* af_d = gVR.aafAimDirWorld[i_hand];
			CVector3<> v3_pos(af_p[0], af_p[1], af_p[2]);
			CVector3<> v3_dir(af_d[0], af_d[1], af_d[2]);
			if (v3_dir.tLenSqr() < 0.01f)
				v3_dir = CVector3<>(0, 1, 0);

			CPlacement3<> p3_cast(CRotate3<>(d3YAxis, CDir3<>(v3_dir)), v3_pos);
			CRayCast rc(p3_cast, 0.25f, fFORCE_RANGE);

			CInstance* pins_found = nullptr;
			const char* str_stop = "nothing";
			SObjectLoc* pobl;
			while (pobl = rc.poblNextIntersection())
			{
				CInstance* pins = pobl->pinsObject;
				if (pins == pinsHand || pins == this || pins == avrh[0].pinsBox || pins == avrh[1].pinsBox ||
					pins == avrh[0].pinsHeld || pins == avrh[1].pinsHeld || ptCast<CBoundaryBox>(pins))
					continue;
				if (bPickupable(pins))
					pins_found = NMagnetSystem::pinsFindMaster(pins);
				else
					str_stop = pins->strGetInstanceName();
				// Stop at the first solid thing.
				break;
			}

			if (pins_found != pins_target)
			{
				pins_target = pins_found;
				if (pins_found)
				{
					gVR.aiForceTargetSerial[i_hand]++;
					if (gVR.pfnLog)
						gVR.pfnLog("Force grab (%s hand): target %s", i_hand ? "left" : "right", pins_found->strGetInstanceName());
				}
			}

			static TSec s_last_log = -100;
			if (!pins_found && gVR.pfnLog && CMessageStep::sStaticTotal - s_last_log > 2.0)
			{
				gVR.pfnLog("Force grab (%s hand): aiming, ray hits %s", i_hand ? "left" : "right", str_stop);
				s_last_log = CMessageStep::sStaticTotal;
			}
		}
		else if (CMessageStep::sStaticTotal - s_pull_time > 2.0)
		{
			pins_target = nullptr;
		}

		gVR.abForceTarget[i_hand] = pins_target != nullptr && gVR.abForceAim[i_hand];

		if (gVR.abForcePull[i_hand])
		{
			gVR.abForcePull[i_hand] = false;
			if (pins_target && CMessageStep::sStaticTotal - s_pull_time > 0.5)
			{
				// Throw the object on a ballistic arc that lands at the hand.
				CPhysicsInfo* pphi = pins_target->pphiGetPhysicsInfoWritable();
				CVector3<> v3_obj  = pins_target->v3Pos();
				CVector3<> v3_hand = vh.pinsBox->v3Pos() + CVector3<>(0, 0, 0.15f);
				CVector3<> v3_delta = v3_hand - v3_obj;
				float f_dist = v3_delta.tLen();
				float f_time = f_dist / 7.0f;
				SetMinMax(f_time, 0.35f, 0.8f);

				CVector3<> v3_vel = v3_delta / f_time + CVector3<>(0, 0, 0.5f * 9.8f * f_time);
				pphi->ForceVelocity(pins_target, v3_vel);
				s_pull_time = CMessageStep::sStaticTotal;
				if (gVR.pfnLog)
					gVR.pfnLog("Force grab (%s hand): pulled %s (%.1f m).", i_hand ? "left" : "right", pins_target->strGetInstanceName(), f_dist);
			}
		}

		// Catching is done by the normal grab: squeezing the grip near it.
	}

	//*****************************************************************************************
	void CPlayerPriv::HandleStow(const CMessageControl& msgc, int i_vr_slot)
	{
		bool b_vr = i_vr_slot >= 0;
		if (msgc.bHit(uCMD_STOW) || b_vr)
		{
			if (!iStowing)
			{
				int i_stowable = iStowable();
				if (b_vr && eHandHolding != ehhEMPTY && i_stowable < 0)
				{
					// VR: holding something too big to holster; ignore.
				}
				else if (b_vr && eHandHolding == ehhEMPTY && !apinsStowed[i_vr_slot])
				{
					// VR: nothing in that holster.
				}
				else if (i_stowable >= 0)
				{
					// Stow this object away, before retrieving any previously stowed object.
					// Initiate stowing state.
					// Go to mid-point first to stow object.
					iStowing = b_vr ? 3 : 1;
					iStowSlot = i_stowable;
					sActionTime = CMessageStep::sStaticTotal;

					// Remember current hand placement; position is head-relative.
					p3Stowing = p3Hand;
					p3Stowing.v3Pos /= p3Head;
				}
				else if (apinsStowed[0] || apinsStowed[1])
				{
					// We have a stowed object to retrieve.
					iStowing = b_vr ? 3 : 1;
					iStowSlot = (b_vr && apinsStowed[i_vr_slot]) ? i_vr_slot : (apinsStowed[0] ? 0 : 1);
					sActionTime = CMessageStep::sStaticTotal;

					// Drop any (unstowable) held object.
					Drop();

					p3Stowing.v3Pos = v3HandBody((PlayerSettings.ang2HandViewMin + PlayerSettings.ang2HandViewMax) * 0.5, PlayerSettings.fHandDistMin);
					p3Stowing.r3Rot = r3Null;
				}
			}
		}

		if (iStowing)
		{
			//
			// See whether we got there yet.
			//

			// Ignore collisions between hand and body while stowing.
			pphSystem->IgnoreCollisions(pinsHand, apbbBoundaryBoxes[(int)ebbBODY], true);
			pphSystem->IgnoreCollisions(pinsHand, apbbBoundaryBoxes[(int)ebbFOOT], true);

			// Target palm placement is stow placement.
			int i_stow_pos = iStowing <= 3 ? iStowing-1 : 5-iStowing;
			CPlacement3<> p3_hand_target = PlayerSettings.aap3Stow[iStowSlot][i_stow_pos];

			// Offset to get equivalent hand position.
			p3_hand_target.v3Pos = v3PalmToHandSpecial(p3_hand_target);

			// Convert back to player request space.
			p3Hand = p3_hand_target;
			p3Hand.r3Rot /= r3HandRequestSpace();
			bHandRotate = true;

			//
			// Check for proximity.
			//

			// Convert to world space.
			p3_hand_target *= p3GetPlacement();

			// Only enforce required orientation when holding an object.
			TSec s_elapsed = CMessageStep::sStaticTotal - sActionTime;
			bool b_check_orient = eHandHolding == ehhHOLDING;
//			if (iStowing == 3)
				// At final position, decrease effective elapsed time, 
				// to require closer proximity than for pickup.
//				s_elapsed *= 0.5;
			if (s_elapsed >= PlayerSettings.sPickupTime ||
				bCloseEnough(pinsHand->p3GetPlacement(), p3_hand_target, s_elapsed, b_check_orient))
			{
				// We just neared intermediate point; now we need to go to next point.
				AlwaysAssert(bWithin(iStowing, 1, 5));
				iStowing++;
				sActionTime = CMessageStep::sStaticTotal;

				if (iStowing == 4)
				{
					// We are at final point; force the hand to the exact position for stow.
					pinsHand->Move(p3_hand_target);

					// Stow the object.
					// Remember currently stowed object.
					CInstance* apins_stowed[2] = { apinsStowed[0], apinsStowed[1] };

					// See whether we have a held stowable object.
					if (eHandHolding == ehhHOLDING)
					{
						// We are stowing this object.
						apinsStowed[iStowSlot] = pinsHeld;

						// Let go of whatever we're holding.
						Drop();

						// Try early to substitute back to the default mesh, so we can pick up.
						AttemptSubstitute(iSubstituteRequest);

						// Magnet it to the player, while at the same time making it intangible.
						NMagnetSystem::AddMagnetPair(this, apinsStowed[iStowSlot], CMagnet::pmagFindShared(CMagnet(Set(emfINTANGIBLE))));

						// Store relative position, to eliminate drift.
						ap3StowedObj[iStowSlot] = apinsStowed[iStowSlot]->p3GetPlacement() / p3GetPlacement();

						CMessagePickUp(apinsStowed[iStowSlot], epuSTOW).Dispatch();
					}
					else
						apinsStowed[iStowSlot] = 0;

					if (apins_stowed[iStowSlot])
					{
						// We must retrieve the previously stowed object.
						// De-magnet previously stowed object from player.
						NMagnetSystem::RemoveMagnetPair(this, apins_stowed[iStowSlot]);

						// Pick up the previously stowed object.
						Pickup(apins_stowed[iStowSlot]);
						CMessagePickUp(pinsHeld, epuRETRIEVE).Dispatch();
					}

					// Make sure hand isn't colliding with other objects.
					ProtectHand(false);

					// Remember preferred hand placement.
					Swap(p3Stowing, p3Stowed);

					if (!apinsStowed[1 - iStowSlot] && eHandHolding == ehhEMPTY)
						// Just stowed, no new object to pick up, all done.
						iStowing = 0;

					if (gVR.bActive && gVR.bMotionHand)
					{
						// VR: the controller takes the hand straight back; no retract animation.
						iStowing = 0;
						if (eHandHolding == ehhHOLDING)
							sRaiseTime = CMessageStep::sStaticTotal + PlayerSettings.sPickupNoDrop;
					}
				}
				else if (iStowing == 6)
				{
					// We retracted back to first point.
					if (apinsStowed[1 - iStowSlot] && eHandHolding == ehhEMPTY)
					{
						// We must retrieve the stowed object from the other slot.
						// Go to mid stow location, and come back when we get there.
						iStowing = 1;
						iStowSlot = 1 - iStowSlot;

						// Remember preferred hand placement.
						Swap(p3Stowing, p3Stowed);
					}
					else
					{
						// We're done with everything.
						if (eHandHolding == ehhHOLDING)
						{
							// Raise it to previously selected position; 
							// position is head-relative, distance shoulder-relative.
							p3Hand = p3Stowing;
							p3Hand.v3Pos *= p3Head;
							AdjustHandDist(rHandDistance());
							bHandRotate = true;

							// Set the time to wait before allowing drop.
							sRaiseTime = CMessageStep::sStaticTotal + PlayerSettings.sPickupNoDrop;
						}
						iStowing = 0;
					}
				}
			}
		} // if (iStowing)
	}

	//*****************************************************************************************
	void CPlayerPriv::HandleSwing(const CMessageControl& msgc, bool b_use)
	{
#if 1
		if (!b_use || !bSwingable())
		{
			iSwinging = 0;
			return;
		}

		if (!iSwinging)
		{
			// Start a-swinging'.
			iSwinging = 1;

			// Set pull-back target here. Player can modify it dynamically.
			// The target is a certain angular distance from the screen centre, through the
			// player's actual hand location.

			// Get current hand position in head space.
			CAngles2 ang2_hand = Angles2(CDir3<>(v3HandActual() / p3Head));

			TReal r_len = ang2_hand.tLen();
			if (r_len < PlayerSettings.angSwingPull)
				// Extend out to desired angle.
				ang2_hand.Normalise(PlayerSettings.angSwingPull);

			// Convert to position, using default arm length.
			SetHandPosHead(ang2_hand);

			// Remember current time.
			sActionTime = CMessageStep::sStaticTotal;
		}

		if (iSwinging)
		{
			// Are we there yet ????.....?????
			TSec s_elapsed = CMessageStep::sStaticTotal - sActionTime;
			bool b_close = bCloseEnough(pinsHand->p3GetPlacement(), p3Hand.v3Pos * p3GetPlacement(), s_elapsed, false);
			if (b_close || s_elapsed > PlayerSettings.sPickupTime)
			{
				// We there. 
				if (iSwinging == 1)
				{
					// Now go for maximal swing to opposite extreme, hopefully hitting something on the way.
					// Again, player can modify this target if she's fast enough.

					// Get target hand position in head space, and move to opposite end.
					CAngles2 ang2_hand = Angles2(CDir3<>(p3Hand.v3Pos / p3Head));
					ang2_hand.Normalise(-PlayerSettings.angSwingPull);

					// Convert to position, using maximum arm length.
					SetHandPosHead(ang2_hand, PlayerSettings.fHandDistMin);

					iSwinging = 2;
					sActionTime = CMessageStep::sStaticTotal;
				}
				else 
				{
					// Done with this swing.
					// Would you like to start another?
					if (b_use)
					{
						//
						// Choose new pull-back target.
						//

						// Get target hand position in head space.
						CAngles2 ang2_hand = Angles2(CDir3<>(p3Hand.v3Pos / p3Head));

						// If the last swing neared its target, use it as the new pull-back target.
						// Otherwise, use its opposite.
						ang2_hand.Normalise(b_close ? PlayerSettings.angSwingPull : -PlayerSettings.angSwingPull);

						// Convert to position, using maximum arm length.
						SetHandPosHead(ang2_hand, PlayerSettings.fHandDistMin);

						// Keep a-swinging'.
						iSwinging = 1;
						sActionTime = CMessageStep::sStaticTotal;
					}
					else
					{
						// Return to a default position.
						CAngles2 ang2_hand = (PlayerSettings.ang2HandViewMin + PlayerSettings.ang2HandViewMax) * 0.5;
						SetHandPosHead(ang2_hand);

						iSwinging = 0;



						// Set the time to wait before allowing drop.
						sRaiseTime = CMessageStep::sStaticTotal + PlayerSettings.sPickupNoDrop;
					}
				}
			}
		}
#else
		if (b_use && bSwingable())
		{
			if (!iSwinging)
			{
				// Start a-swinging'.
				iSwinging = 1;

				// Set pull-back target here. Player can modify it dynamically.
				// The target is a certain angular distance from the screen centre, through the
				// player's actual hand location.

				// Get current hand position in head space.
				CAngles2 ang2_hand = Angles2(CDir3<>(v3HandActual() / p3Head));

				TReal r_len = ang2_hand.tLen();
				if (r_len < PlayerSettings.angSwingPull)
					// Extend out to desired angle.
					ang2_hand.Normalise(PlayerSettings.angSwingPull);

				// Convert to position, using default arm length.
				SetHandPosHead(ang2_hand);

				// Remember current time.
				sActionTime = CMessageStep::sStaticTotal;
			}

			if (iSwinging)
			{
				// Are we there yet ????.....????? 
				// Make sure hand is in the same general quadrant we want it, and
				// check distance from centre.
				CAngles2 ang2_targ = Angles2(CDir3<>(p3Hand.v3Pos / p3Head));
				CAngles2 ang2_hand = Angles2(CDir3<>(v3HandActual() / p3Head));
				TReal r_len = ang2_hand.tLen();
				if ((ang2_targ * ang2_hand > 0.0f && r_len >= PlayerSettings.angSwingPull) ||
					CMessageStep::sStaticTotal - sActionTime >= PlayerSettings.sSwingTime)
				{
					// We got there, or timed out. 
					// Now go for maximal swing to opposite extreme, hopefully hitting something on the way.
					// Again, player can modify this target if she's fast enough.

					// Get target hand position in head space.
					CAngles2 ang2_hand = Angles2(CDir3<>(v3HandActual() / p3Head));
					ang2_hand.Normalise(-PlayerSettings.angSwingPull);

					// Convert to position, using default arm length.
					SetHandPosHead(ang2_hand);

					sActionTime = CMessageStep::sStaticTotal;
				}
			}
		}
		// if can and will swing.
		else
		{
			// Can't or won't swing.
			if (iSwinging)
			{
				iSwinging = 0;

				// Return to a default position.
				CAngles2 ang2_hand = (PlayerSettings.ang2HandViewMin + PlayerSettings.ang2HandViewMax) * 0.5;
				SetHandPosHead(ang2_hand);

				// Set the time to wait before allowing drop.
				sRaiseTime = CMessageStep::sStaticTotal + PlayerSettings.sPickupNoDrop;
			}
		}
#endif
	}

	//*****************************************************************************************
	void CPlayerPriv::Say(TSoundHandle sndhnd)
	{
		if (!bCanTalk() || !sndhnd)
			return;

		// Okay, here begins some hacking.
		bool b_played_sound = false;
		SSoundEffectResult ser;

		if (bQuerySoundEffect(sndhnd, &ser))
		{

			CMessageAudio	msg
				(
					sndhnd,
					eamATTACHED_EFFECT,			// type of sound
					NULL,						// sender (NULL if using play function)
					padAudioDaemon,				// receiver (NULL if using play function)
					this,						// parent of the effect (can be NULL for stereo)
					0.0f,						// volume 0dBs
					0.0f,						// attenuation (only for pseudo/real 3D samples)
					AU_STEREO,					// spatial type
					360.0f,						// fustrum angle (real 3D only)
					-15.0f,						// outside volume (real 3D only)
					false,						// looped
					0,							// loop count (pseudo/real 3D only)
					10000.0f					// distance before sound is stopped (pseudo/real 3D only)
				);
			b_played_sound = bImmediateSoundEffect(msg, &ser);
		}

		// Did we play a sound?
		if (b_played_sound)
		{	
			// Record the sample length.
			sDoneTalking = CMessageStep::sStaticTotal + ser.fPlayTime;

			// Record the ID.
			u4AudioID = ser.u4SoundID;

#if VER_TEST
			// BRADY   change this to "true" to get your printouts!
			if (false)
			{
				char buffer[32];
				sprintf(buffer, "%x\n", sndhnd);

				dout << "Player played " << buffer;
			}
#endif
		}
	}

	//*****************************************************************************************
	void CPlayerPriv::Say(const TSampleSet* pss)
	{
		if (pss->uLen > 0)
		{
			Say((*pss)[rnd() % pss->uLen]);
		}
	}

//******************************************************************************************
//
// class CPlayer implementation.
//

	//**************************************************************************************
	CPlayer* CPlayer::pplayNew()
	{
		return new CPlayerPriv();
	}

	//**************************************************************************************
	CPlayer::~CPlayer()
	{
	}
