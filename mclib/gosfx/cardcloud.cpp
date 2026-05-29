//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include"gosfxheaders.hpp"
#include<mlr/mlrcardcloud.hpp>

// B1 Stage 2' C11: subclass-Start routing into the GPU particle pipeline.
#include"particles/batcher.h"
#include"particles/spawn.h"
#include<cstdio>   // VFX-ORACLE diagnostics

//------------------------------------------------------------------------------
//
gosFX::CardCloud__Specification::CardCloud__Specification(
	Stuff::MemoryStream *stream,
	int gfx_version
):
	SpinningCloud__Specification(gosFX::CardCloudClassID, stream, gfx_version)
{
	Check_Pointer(this);
	Check_Object(stream);
	Verify(m_class == CardCloudClassID);
	Verify(gos_GetCurrentHeap() == Heap);

	m_halfHeight.Load(stream, gfx_version);
	m_aspectRatio.Load(stream, gfx_version);

	//
	//-------------------------------------------------------------------
	// If we are reading an old version of the card cloud, ignore all the
	// animation on the UV channels
	//-------------------------------------------------------------------
	//
	if (gfx_version < 10)
	{
		m_pIndex.m_ageCurve.SetCurve(0.0f);
		m_pIndex.m_seedCurve.SetCurve(1.0f);
		m_pIndex.m_seeded = false;

		SeededCurveOf<ComplexCurve, LinearCurve,Curve::e_ComplexLinearType> temp;
		temp.Load(stream, gfx_version);
		Stuff::Scalar v = temp.ComputeValue(0.0f, 0.0f);
		m_UOffset.SetCurve(v);

		temp.Load(stream, gfx_version);
		v = temp.ComputeValue(0.0f, 0.0f);
		m_VOffset.SetCurve(v);

		m_USize.Load(stream, gfx_version);
		m_VSize.Load(stream, gfx_version);

		m_animated = false;
	}

	//
	//------------------------------
	// Otherwise, read in the curves
	//------------------------------
	//
	else
	{
		m_pIndex.Load(stream, gfx_version);
		m_UOffset.Load(stream, gfx_version);
		m_VOffset.Load(stream, gfx_version);
		m_USize.Load(stream, gfx_version);
		m_VSize.Load(stream, gfx_version);
		*stream >> m_animated;
	}
	SetWidth();

	m_totalParticleSize = gosFX::CardCloud::ParticleSize;
	m_particleClassSize = sizeof(gosFX::CardCloud::Particle);
}

//------------------------------------------------------------------------------
//
gosFX::CardCloud__Specification::CardCloud__Specification():
	SpinningCloud__Specification(gosFX::CardCloudClassID)
{
	Check_Pointer(this);
	Verify(gos_GetCurrentHeap() == Heap);
	m_animated = false;
	m_width = 1;
	m_totalParticleSize = gosFX::CardCloud::ParticleSize;
	m_particleClassSize = sizeof(gosFX::CardCloud::Particle);
}

//------------------------------------------------------------------------------
//
gosFX::CardCloud__Specification*
	gosFX::CardCloud__Specification::Make(
		Stuff::MemoryStream *stream,
		int gfx_version
	)
{
	Check_Object(stream);

	gos_PushCurrentHeap(Heap);
	CardCloud__Specification *spec =
		new gosFX::CardCloud__Specification(stream, gfx_version);
	gos_PopCurrentHeap();

	return spec;
}

//------------------------------------------------------------------------------
//
void
	gosFX::CardCloud__Specification::Save(Stuff::MemoryStream *stream)
{
	Check_Object(this);
	Check_Object(stream);
	SpinningCloud__Specification::Save(stream);

	m_halfHeight.Save(stream);
	m_aspectRatio.Save(stream);
	m_pIndex.Save(stream);
	m_UOffset.Save(stream);
	m_VOffset.Save(stream);
	m_USize.Save(stream);
	m_VSize.Save(stream);
	*stream << m_animated;
}
//------------------------------------------------------------------------------
//

void 
	gosFX::CardCloud__Specification::BuildDefaults()
{

	Check_Object(this);
	SpinningCloud__Specification::BuildDefaults();

	m_halfHeight.m_ageCurve.SetCurve(1.0f);
	m_halfHeight.m_seeded = false;
	m_halfHeight.m_seedCurve.SetCurve(1.0f);

	m_aspectRatio.m_ageCurve.SetCurve(1.0f);
	m_aspectRatio.m_seeded = false;
	m_aspectRatio.m_seedCurve.SetCurve(1.0f);

	m_pIndex.m_ageCurve.SetCurve(0.0f);
	m_pIndex.m_seeded = false;
	m_pIndex.m_seedCurve.SetCurve(1.0f);

	m_UOffset.SetCurve(0.0f);
	m_VOffset.SetCurve(0.0f);
	m_USize.SetCurve(1.0f);
	m_VSize.SetCurve(1.0f);

	m_animated = false;
	m_width = 1;
}

//------------------------------------------------------------------------------
//

bool 
	gosFX::CardCloud__Specification::IsDataValid(bool fix_data)
{

	Check_Object(this);

		Stuff::Scalar max_offset, min_offset;
	Stuff::Scalar max_scale, min_scale;
	m_USize.ExpensiveComputeRange(&min_scale, &max_scale);
	Stuff::Scalar lower = min_scale;
	if (lower > 0.0f)
		lower = 0.0f;
	Stuff::Scalar upper = max_scale;

	//
	//------------------------------------
	// Calculate the worst case UV offsets
	//------------------------------------
	//
	m_VOffset.ExpensiveComputeRange(&min_offset, &max_offset);
	lower += min_offset;
	upper += max_offset;

	if (upper > 99.0f || lower < -99.0f)
	{
		if(fix_data)
		{
			m_VOffset.SetCurve(0.0f);
			PAUSE(("Warning: Curve \"VOffset\" in Effect \"%s\" Is Out of Range and has been Reset",(char *)m_name));

		}
			else
		return false;
	}
	
	m_VSize.ExpensiveComputeRange(&min_scale, &max_scale);
	lower = min_scale;
	if (lower > 0.0f)
		lower = 0.0f;
	upper = max_scale;

	//
	//------------------------------------
	// Calculate the worst case UV offsets
	//------------------------------------
	//
	//max_offset, min_offset;
	m_UOffset.ExpensiveComputeRange(&min_offset, &max_offset);
	lower += min_offset;
	upper += max_offset;

	if (upper > 99.0f || lower < -99.0f)
	{
		if(fix_data)
		{
			m_UOffset.SetCurve(0.0f);
			PAUSE(("Warning: Curve \"UOffset\" in Effect \"%s\" Is Out of Range and has been Reset",(char *)m_name));

		}
		else
			return false;
	}

	
	return	SpinningCloud__Specification::IsDataValid(fix_data);

}


//------------------------------------------------------------------------------
//
void
	gosFX::CardCloud__Specification::Copy(CardCloud__Specification *spec)
{
	Check_Object(this);
	Check_Object(spec);

	SpinningCloud__Specification::Copy(spec);

	gos_PushCurrentHeap(Heap);
	m_halfHeight = spec->m_halfHeight;
	m_aspectRatio = spec->m_aspectRatio;
	m_pIndex = spec->m_pIndex;
	m_UOffset = spec->m_UOffset;
	m_VOffset = spec->m_VOffset;
	m_USize = spec->m_USize;
	m_VSize = spec->m_VSize;
	m_animated = spec->m_animated;
	m_width = spec->m_width;
	gos_PopCurrentHeap();
}

//------------------------------------------------------------------------------
//
void
	gosFX::CardCloud__Specification::SetWidth()
{
	m_width =
		static_cast<BYTE>(1.0f / m_USize.ComputeValue(0.0f, 0.0f));
}

//############################################################################
//##############################  gosFX::CardCloud  ################################
//############################################################################

gosFX::CardCloud::ClassData*
	gosFX::CardCloud::DefaultData = NULL;

//------------------------------------------------------------------------------
//
void
	gosFX::CardCloud::InitializeClass()
{
	Verify(!DefaultData);
	Verify(gos_GetCurrentHeap() == Heap);
	DefaultData =
		new ClassData(
			CardCloudClassID,
			"gosFX::CardCloud",
			SpinningCloud::DefaultData,
			(Effect::Factory)&Make,
			(Specification::Factory)&Specification::Make
		);
	Register_Object(DefaultData);
}

//------------------------------------------------------------------------------
//
void
	gosFX::CardCloud::TerminateClass()
{
	Unregister_Object(DefaultData);
	delete DefaultData;
	DefaultData = NULL;
}

//------------------------------------------------------------------------------
//
gosFX::CardCloud::CardCloud(
	Specification *spec,
	unsigned flags
):
	SpinningCloud(DefaultData, spec, flags)
{
	Check_Object(spec);
	Verify(gos_GetCurrentHeap() == Heap);

	gos_PushCurrentHeap(MidLevelRenderer::Heap);
	m_cloudImplementation =
		new MidLevelRenderer::MLRCardCloud(spec->m_maxParticleCount);
	Register_Object(m_cloudImplementation);
	gos_PopCurrentHeap();

	unsigned index = spec->m_maxParticleCount*sizeof(Particle);
	m_P_vertices = Cast_Pointer(Stuff::Point3D*, &m_data[index]);
	index += 4*spec->m_maxParticleCount*sizeof(Stuff::Point3D);
	m_P_color = Cast_Pointer(Stuff::RGBAColor*, &m_data[index]);
	index += spec->m_maxParticleCount * sizeof(Stuff::RGBAColor);
	m_P_uvs = Cast_Pointer(Stuff::Vector2DOf<Stuff::Scalar>*, &m_data[index]);

	m_cloudImplementation->SetData(
		Cast_Pointer(const int *, &m_activeParticleCount),
		m_P_vertices,
		m_P_color,
		m_P_uvs
	);
}

//------------------------------------------------------------------------------
//
gosFX::CardCloud::~CardCloud()
{
	Unregister_Object(m_cloudImplementation);
	delete m_cloudImplementation;
}

//------------------------------------------------------------------------------
//
gosFX::CardCloud*
	gosFX::CardCloud::Make(
		Specification *spec,
		unsigned flags
	)
{
	Check_Object(spec);

	gos_PushCurrentHeap(Heap);
	CardCloud *cloud = new gosFX::CardCloud(spec, flags);
	gos_PopCurrentHeap();

	return cloud;
}

//------------------------------------------------------------------------------
//
void
	gosFX::CardCloud::CreateNewParticle(
		unsigned index,
		Stuff::Point3D *translation
	)
{
	Check_Object(this);

	//
	//-------------------------------------------------------------------
	// Let our parent do creation, then turn on the particle in the cloud
	//-------------------------------------------------------------------
	//
	SpinningCloud::CreateNewParticle(index, translation);
	m_cloudImplementation->TurnOn(index);
	Verify(m_cloudImplementation->IsOn(index));

	//
	//-----------------------------
	// Figure out the particle size
	//-----------------------------
	//
	Specification *spec = GetSpecification();
	Check_Object(spec);
	Particle *particle = GetParticle(index);
	Check_Object(particle);
	particle->m_halfY =
		spec->m_halfHeight.ComputeValue(m_age, particle->m_seed);
	particle->m_halfX =
		particle->m_halfY * spec->m_aspectRatio.ComputeValue(m_age, particle->m_seed);
	particle->m_radius =
		Stuff::Sqrt(
			particle->m_halfX * particle->m_halfX
			 + particle->m_halfY * particle->m_halfY
		);
}

//------------------------------------------------------------------------------
//
bool
	gosFX::CardCloud::AnimateParticle(
		unsigned index,
		const Stuff::LinearMatrix4D *world_to_new_local,
		Stuff::Time till
	)
{
	Check_Object(this);

	//
	//-----------------------------------------
	// Animate the parent then get our pointers
	//-----------------------------------------
	//
	if (!SpinningCloud::AnimateParticle(index, world_to_new_local, till))
		return false;
	Set_Statistic(Card_Count, Card_Count+1);
	Specification *spec = GetSpecification();
	Check_Object(spec);
	Particle *particle = GetParticle(index);
	Check_Object(particle);
	Stuff::Scalar seed = particle->m_seed;
	Stuff::Scalar age = particle->m_age;

	//
	//------------------
	// Animate the color
	//------------------
	//
	Check_Pointer(m_P_color);
	m_P_color[index].red = spec->m_pRed.ComputeValue(age, seed);
	m_P_color[index].green = spec->m_pGreen.ComputeValue(age, seed);
	m_P_color[index].blue = spec->m_pBlue.ComputeValue(age, seed);
	m_P_color[index].alpha = spec->m_pAlpha.ComputeValue(age, seed);

	//
	//----------------
	// Animate the uvs
	//----------------
	//
	Stuff::Scalar u = spec->m_UOffset.ComputeValue(age, seed);
	Stuff::Scalar v = spec->m_VOffset.ComputeValue(age, seed);
	Stuff::Scalar u2 = spec->m_USize.ComputeValue(age, seed);
	Stuff::Scalar v2 = spec->m_VSize.ComputeValue(age, seed);

	//
	//--------------------------------------------------------------
	// If we are animated, figure out the row/column to be displayed
	//--------------------------------------------------------------
	//
	if (spec->m_animated)
	{
		BYTE columns =
			Stuff::Truncate_Float_To_Byte(
				spec->m_pIndex.ComputeValue(age, seed)
			);
		BYTE rows = static_cast<BYTE>(columns / spec->m_width);
		columns = static_cast<BYTE>(columns - rows*spec->m_width);

		//
		//---------------------------
		// Now compute the end points
		//---------------------------
		//
		u += u2*columns;
		v += v2*rows;
	}
	u2 += u;
	v2 += v;

	index *= 4;
	m_P_uvs[index].x = u;
	m_P_uvs[index].y = v2;
	m_P_uvs[++index].x = u2;
	m_P_uvs[index].y = v2;
	m_P_uvs[++index].x = u2;
	m_P_uvs[index].y = v;
	m_P_uvs[++index].x = u;
	m_P_uvs[index].y = v;
	return true;
}

//------------------------------------------------------------------------------
//
void gosFX::CardCloud::DestroyParticle(unsigned index)
{
	Check_Object(this);

	m_cloudImplementation->TurnOff(index);
	Verify(!m_cloudImplementation->IsOn(index));
	SpinningCloud::DestroyParticle(index);
}

//------------------------------------------------------------------------------
//
void gosFX::CardCloud::Draw(DrawInfo *info)
{
	Check_Object(this);
	Check_Object(info);

	// GPU render path (Stage 2'): re-emit current cloud to the batcher on
	// every Draw() call. This matches the legacy path which submits card
	// geometry to MLR render lists each frame. SpawnCardCloud samples spec
	// curves at resolveSampleAge(m_age) (0.5 unless MC2_VFX_AGE_SAMPLE=1)
	// (per-frame CPU-animated positions are a B2 polish item).
	// SpinningCloud::Draw propagates up the chain for EffectCloud children;
	// it does not render card geometry.
	//
	// NOTE: GPU spawn moved from Start() to here. Start()-based emission only
	// filled the batcher for one frame (until the first Flush()), leaving the
	// batcher empty for all subsequent frames.
	if (mc2::particles::Batcher::is_enabled()) {
		if (mc2::particles::Batcher::is_oracle_render_enabled()) {
			// VFX-ORIGINAL-RECORD-ABI-1 (Phase 1, CardCloud): CPU-oracle render.
			// ParticleCloud::Execute (which runs every frame under GPU mode)
			// keeps each live particle current: m_localTranslation and m_scale
			// are advanced per-frame, m_P_color[i] is recomputed in
			// CardCloud::AnimateParticle, and m_halfX/Y are per-particle
			// birth-time extents (set in CreateNewParticle). All are valid at Draw.
			// Emit one GPU billboard per LIVE particle from that real data. CPU
			// sim stays authoritative; the legacy MLR draw stays skipped (no
			// dual-draw). Per-particle rotation (m_localRotation), aspect
			// (halfX vs halfY) and per-particle UV frame are NOT representable in
			// the existing 64B GpuParticle record — deferred to the ABI-extension
			// follow-up (docs/vfx-originals-restoration-design.md).
			if (m_activeParticleCount > 0 && m_P_color) {
				Specification *spec = GetSpecification();
				Check_Object(spec);
				const uint32_t mlrTex =
					static_cast<uint32_t>(spec->m_state.GetTextureHandle());
				int blendMode = 0;
				{
					const MidLevelRenderer::MLRState::AlphaMode am =
						spec->m_state.GetAlphaMode();
					if (am == MidLevelRenderer::MLRState::OneOneMode ||
					    am == MidLevelRenderer::MLRState::AlphaOneMode)
						blendMode = 1;
				}
				// Same effect->world transform the legacy CardCloud::Draw feeds
				// to MLR as effectToWorld.
				Stuff::LinearMatrix4D local_to_world;
				local_to_world.Multiply(m_localToParent, *info->m_parentToWorld);

				mc2::particles::Batcher &batcher = mc2::particles::Batcher::Instance();
				batcher.BeginGroup(mlrTex, 0.0f, 0.0f, 1.0f, 1.0f, blendMode);

				int harvested = 0;
				float minA =  3.0e38f, maxA = -3.0e38f;
				for (int i = 0; i < m_activeParticleCount; ++i) {
					Particle *p = GetParticle(i);
					Check_Object(p);
					if (p->m_age >= 1.0f) continue;   // skip dead slots (legacy filter)
					Stuff::Point3D wc;
					wc.Multiply(p->m_localTranslation, local_to_world);
					// Billboard radius from per-particle scaled half-extents.
					// Aspect (halfX vs halfY) collapsed to a bounding radius —
					// same idiom as the placeholder; per-particle aspect deferred.
					const Stuff::Scalar radius =
						p->m_scale *
						Stuff::Sqrt(p->m_halfX * p->m_halfX + p->m_halfY * p->m_halfY);
					const Stuff::RGBAColor &c = m_P_color[i];
					mc2::particles::GpuParticle gp = {};
					gp.position[0] = wc.x;
					gp.position[1] = wc.y;
					gp.position[2] = wc.z;
					gp.color[0]    = c.red;
					gp.color[1]    = c.green;
					gp.color[2]    = c.blue;
					gp.color[3]    = c.alpha;
					gp.size        = static_cast<float>(radius);
					gp.atlasIndex  = mlrTex;
					batcher.Emit(gp);
					++harvested;
					if (c.alpha < minA) minA = c.alpha;
					if (c.alpha > maxA) maxA = c.alpha;
				}

				// [VFX_ORACLE v1] diagnostics. harvested = LIVE particle count
				// (active minus already-dead m_age>=1 slots) == emitted; no
				// fabrication, no fallback. One-shot on first harvest so sparse
				// clouds are provable; 240-call summary thereafter.
				if (mc2::particles::Batcher::is_log_enabled()) {
					static bool s_first = false;
					if (!s_first && harvested > 0) {
						s_first = true;
						std::fprintf(stderr,
							"[VFX_ORACLE v1] class=CardCloud FIRST_HARVEST active=%d harvested=%d alpha=[%.3f,%.3f]\n",
							m_activeParticleCount, harvested,
							static_cast<double>(minA), static_cast<double>(maxA));
						std::fflush(stderr);
					}
					static unsigned long long s_calls = 0, s_harvTotal = 0;
					s_harvTotal += static_cast<unsigned>(harvested);
					if ((++s_calls % 240ull) == 0ull) {
						// minA/maxA are sentinels if this call harvested 0 live
						// particles; report 0 in that case (avoid 3e38 in the log).
						const double aLo = (harvested > 0) ? static_cast<double>(minA) : 0.0;
						const double aHi = (harvested > 0) ? static_cast<double>(maxA) : 0.0;
						std::fprintf(stderr,
							"[VFX_ORACLE v1] class=CardCloud calls=%llu active_this_call=%d harvested_this_call=%d emitted_this_call=%d harvestedTotal=%llu fallback=0 alpha_this_call=[%.3f,%.3f]\n",
							s_calls, m_activeParticleCount, harvested, harvested,
							s_harvTotal, aLo, aHi);
						std::fflush(stderr);
					}
				}
			}
		} else {
			(void)mc2::particles::Spawn(GetSpecification(), &m_localToWorld, (float)m_seed, (float)m_age);
		}
		SpinningCloud::Draw(info);
		return;
	}

	//
	//---------------------------------------------------------
	// If we have active particles, set up the draw information
	//---------------------------------------------------------
	//
	if (m_activeParticleCount)
	{
		MidLevelRenderer::DrawEffectInformation dInfo;
		dInfo.effect = m_cloudImplementation;
		Specification *spec = GetSpecification();
		Check_Object(spec);
		dInfo.state.Combine(info->m_state, spec->m_state);
		dInfo.clippingFlags = info->m_clippingFlags;
		Stuff::LinearMatrix4D local_to_world;
		local_to_world.Multiply(m_localToParent, *info->m_parentToWorld);
		dInfo.effectToWorld = &local_to_world;

		//
		//--------------------------------------------------------------
		// Check the orientation mode.  The first case is XY orientation
		//--------------------------------------------------------------
		//
		unsigned i;
		unsigned vert=0;
		if (spec->m_alignZUsingX)
		{
			if (spec->m_alignZUsingY)
			{
				//
				//-----------------------------------------
				// Get the camera location into local space
				//-----------------------------------------
				//
				Stuff::Point3D
					camera_in_world(info->m_clipper->GetCameraToWorldMatrix());
				Stuff::Point3D camera_in_cloud;
				camera_in_cloud.MultiplyByInverse(
					camera_in_world,
					local_to_world
				);

				//
				//--------------------------------------
				// Spin through all the active particles
				//--------------------------------------
				//
				for (i = 0; i < m_activeParticleCount; i++)
				{
					Particle *particle = GetParticle(i);
					Check_Object(particle);
					if (particle->m_age < 1.0f)
					{

						//
						//--------------------------------
						// Build the local to cloud matrix
						//--------------------------------
						//
						Stuff::Vector3D direction_in_cloud;
						direction_in_cloud.Subtract(
							camera_in_cloud,
							particle->m_localTranslation
						);
						Stuff::LinearMatrix4D card_to_cloud;
						card_to_cloud.BuildRotation(particle->m_localRotation);
						card_to_cloud.AlignLocalAxisToWorldVector(
							direction_in_cloud,
							Stuff::Z_Axis,
							Stuff::Y_Axis,
							Stuff::X_Axis
						);
						card_to_cloud.BuildTranslation(
							particle->m_localTranslation
						);

						//
						//-------------------------------------------------
						// Figure out the scale, then build the four points
						//-------------------------------------------------
						//
						Stuff::Scalar scale = particle->m_scale;
						m_P_vertices[vert++].Multiply(
							Stuff::Point3D(
								scale*particle->m_halfX,
								-scale*particle->m_halfY,
								0.0f
							),
							card_to_cloud
						);
						m_P_vertices[vert++].Multiply(
							Stuff::Point3D(
								-scale*particle->m_halfX,
								-scale*particle->m_halfY,
								0.0f
							),
							card_to_cloud
						);
						m_P_vertices[vert++].Multiply(
							Stuff::Point3D(
								-scale*particle->m_halfX,
								scale*particle->m_halfY,
								0.0f
							),
							card_to_cloud
						);
						m_P_vertices[vert++].Multiply(
							Stuff::Point3D(
								scale*particle->m_halfX,
								scale*particle->m_halfY,
								0.0f
							),
							card_to_cloud
						);
					}
					else
						vert += 4;
				}
			}

			//
			//-----------------------
			// Handle X-only rotation
			//-----------------------
			//
			else
			{
				//
				//-----------------------------------------
				// Get the camera location into local space
				//-----------------------------------------
				//
				Stuff::Point3D
					camera_in_world(info->m_clipper->GetCameraToWorldMatrix());
				Stuff::Point3D camera_in_cloud;
				camera_in_cloud.MultiplyByInverse(
					camera_in_world,
					local_to_world
				);

				//
				//--------------------------------------
				// Spin through all the active particles
				//--------------------------------------
				//
				for (i = 0; i < m_activeParticleCount; i++)
				{
					Particle *particle = GetParticle(i);
					Check_Object(particle);
					if (particle->m_age < 1.0f)
					{

						//
						//--------------------------------
						// Build the local to cloud matrix
						//--------------------------------
						//
						Stuff::Vector3D direction_in_cloud;
						direction_in_cloud.Subtract(
							camera_in_cloud,
							particle->m_localTranslation
						);
						Stuff::LinearMatrix4D card_to_cloud;
						card_to_cloud.BuildRotation(particle->m_localRotation);
						card_to_cloud.AlignLocalAxisToWorldVector(
							direction_in_cloud,
							Stuff::Z_Axis,
							Stuff::X_Axis,
							-1
						);
						card_to_cloud.BuildTranslation(particle->m_localTranslation);

						//
						//-------------------------------------------------
						// Figure out the scale, then build the four points
						//-------------------------------------------------
						//
						Stuff::Scalar scale = particle->m_scale;
						m_P_vertices[vert++].Multiply(
							Stuff::Point3D(
								scale*particle->m_halfX,
								-scale*particle->m_halfY,
								0.0f
							),
							card_to_cloud
						);
						m_P_vertices[vert++].Multiply(
							Stuff::Point3D(
								-scale*particle->m_halfX,
								-scale*particle->m_halfY,
								0.0f
							),
							card_to_cloud
						);
						m_P_vertices[vert++].Multiply(
							Stuff::Point3D(
								-scale*particle->m_halfX,
								scale*particle->m_halfY,
								0.0f
							),
							card_to_cloud
						);
						m_P_vertices[vert++].Multiply(
							Stuff::Point3D(
								scale*particle->m_halfX,
								scale*particle->m_halfY,
								0.0f
							),
							card_to_cloud
						);
					}
					else
						vert += 4;
				}
			}
		}

		//
		//-------------------------------------------------------
		// Each matrix needs to be aligned to the camera around Y
		//-------------------------------------------------------
		//
		else if (spec->m_alignZUsingY)
		{
			//
			//-----------------------------------------
			// Get the camera location into local space
			//-----------------------------------------
			//
			Stuff::Point3D
				camera_in_world(info->m_clipper->GetCameraToWorldMatrix());
			Stuff::Point3D camera_in_cloud;
			camera_in_cloud.MultiplyByInverse(
				camera_in_world,
				local_to_world
			);

			//
			//--------------------------------------
			// Spin through all the active particles
			//--------------------------------------
			//
			for (i = 0; i < m_activeParticleCount; i++)
			{
				Particle *particle = GetParticle(i);
				Check_Object(particle);
				if (particle->m_age < 1.0f)
				{

					//
					//--------------------------------
					// Build the local to cloud matrix
					//--------------------------------
					//
					Stuff::Vector3D direction_in_cloud;
					direction_in_cloud.Subtract(
						camera_in_cloud,
						particle->m_localTranslation
					);
					Stuff::LinearMatrix4D card_to_cloud;
					card_to_cloud.BuildRotation(particle->m_localRotation);
					card_to_cloud.AlignLocalAxisToWorldVector(
						direction_in_cloud,
						Stuff::Z_Axis,
						Stuff::Y_Axis,
						-1
					);
					card_to_cloud.BuildTranslation(particle->m_localTranslation);

					//
					//-------------------------------------------------
					// Figure out the scale, then build the four points
					//-------------------------------------------------
					//
					Stuff::Scalar scale = particle->m_scale;
					m_P_vertices[vert++].Multiply(
						Stuff::Point3D(
							scale*particle->m_halfX,
							-scale*particle->m_halfY,
							0.0f
						),
						card_to_cloud
					);
					m_P_vertices[vert++].Multiply(
						Stuff::Point3D(
							-scale*particle->m_halfX,
							-scale*particle->m_halfY,
							0.0f
						),
						card_to_cloud
					);
					m_P_vertices[vert++].Multiply(
						Stuff::Point3D(
							-scale*particle->m_halfX,
							scale*particle->m_halfY,
							0.0f
						),
						card_to_cloud
					);
					m_P_vertices[vert++].Multiply(
						Stuff::Point3D(
							scale*particle->m_halfX,
							scale*particle->m_halfY,
							0.0f
						),
						card_to_cloud
					);
				}
				else
					vert += 4;
			}
		}

		//
		//---------------------------------------------------------------
		// No alignment is necessary, so just multiply out all the active
		// particles
		//---------------------------------------------------------------
		//
		else
		{
			for (i = 0; i < m_activeParticleCount; i++)
			{
				Particle *particle = GetParticle(i);
				Check_Object(particle);
				if (particle->m_age < 1.0f)
				{

					//
					//--------------------------------
					// Build the local to cloud matrix
					//--------------------------------
					//
					Stuff::LinearMatrix4D card_to_cloud;
					card_to_cloud.BuildRotation(particle->m_localRotation);
					card_to_cloud.BuildTranslation(particle->m_localTranslation);

					//
					//-------------------------------------------------
					// Figure out the scale, then build the four points
					//-------------------------------------------------
					//
					Stuff::Scalar scale = particle->m_scale;
					m_P_vertices[vert++].Multiply(
						Stuff::Point3D(
							scale*particle->m_halfX,
							-scale*particle->m_halfY,
							0.0f
						),
						card_to_cloud
					);
					m_P_vertices[vert++].Multiply(
						Stuff::Point3D(
							-scale*particle->m_halfX,
							-scale*particle->m_halfY,
							0.0f
						),
						card_to_cloud
					);
					m_P_vertices[vert++].Multiply(
						Stuff::Point3D(
							-scale*particle->m_halfX,
							scale*particle->m_halfY,
							0.0f
						),
						card_to_cloud
					);
					m_P_vertices[vert++].Multiply(
						Stuff::Point3D(
							scale*particle->m_halfX,
							scale*particle->m_halfY,
							0.0f
						),
						card_to_cloud
					);
				}
				else
					vert += 4;
			}
		}

		//
		//---------------------
		// Now just do the draw
		//---------------------
		//
	 	info->m_clipper->DrawEffect(&dInfo);
	}

	SpinningCloud::Draw(info);
}

//------------------------------------------------------------------------------
//
void
	gosFX::CardCloud::TestInstance() const
{
	Verify(IsDerivedFrom(DefaultData));
}

//------------------------------------------------------------------------------
// B1 Stage 2' C11 — route to GPU particle pipeline when env-gated on.
// See pointcloud.cpp Start for the full rationale; same pattern as
// ShardCloud (C5/C8). C10 diagnostic showed CardCloud is 45.9% of stock
// Effect::Start calls in mc2_10 — the dominant per-class spawn type.
//
void
	gosFX::CardCloud::Start(ExecuteInfo *info)
{
	Check_Object(this);
	Check_Pointer(info);

	// C9 fix: ALWAYS call SpinningCloud::Start (which resolves to the
	// inherited ParticleCloud::Start) so per-particle structures are
	// initialized. Skipping the parent under env-on corrupts heap state
	// because the destructor and legacy Execute walk garbage memory.
	SpinningCloud::Start(info);

	// GPU spawn moved to Draw() for per-frame emission. Start()-based spawning
	// only filled the batcher for a single frame; Draw() re-emits each frame
	// while the effect is alive, matching the legacy MLR submission cadence.
}
