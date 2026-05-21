#include"gosfxheaders.hpp"
#include"particles/spec_library.h"
#include"particles/batcher.h"
#include"particles/effect_adapter.h"

//==========================================================================//
// File:	 gosFX_Effect.cpp												//
// Contents: Base gosFX::Effect Component									//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//
// B1 Stage 2' C1: parser/lookup body extracted to
// mc2::particles::SpecLibrary (mclib/particles/spec_library.{h,cpp}).
// This file is now the gosFX-namespace forwarding adapter. m_effects
// ownership moved to SpecLibrary; FX_TRACE_SPAWN emit site moved with
// Find() to spec_library.cpp.
//

gosFX::EffectLibrary*
	gosFX::EffectLibrary::Instance = NULL;

//------------------------------------------------------------------------------
//
void
	gosFX::EffectLibrary::InitializeClass()
{
	Verify(!Instance);
}

//------------------------------------------------------------------------------
//
void
	gosFX::EffectLibrary::TerminateClass()
{
	if (Instance)
	{
		Unregister_Object(Instance);
		delete Instance;
		Instance=NULL;
	}
	// Tear down the underlying SpecLibrary so a subsequent mission load
	// reparses cleanly. Safe to call repeatedly.
	mc2::particles::SpecLibrary::Shutdown();
}

//------------------------------------------------------------------------------
//
gosFX::EffectLibrary::EffectLibrary()
{
	Verify(gos_GetCurrentHeap() == Heap);
	// Lazy-init the underlying spec library so producer-side singleton-init
	// at the existing call sites (code/mechcmd2.cpp:1657, Viewer/View.cpp:550,
	// mclib/txmmgr.cpp:522) wires SpecLibrary without touching those sites.
	mc2::particles::SpecLibrary::Instance();
}

//------------------------------------------------------------------------------
//
gosFX::EffectLibrary::~EffectLibrary()
{
	// Storage lives on SpecLibrary; nothing to free here. SpecLibrary is
	// torn down in TerminateClass() above.
}

//------------------------------------------------------------------------------
//
void
	gosFX::EffectLibrary::Load(Stuff::MemoryStream* stream)
{
	mc2::particles::SpecLibrary::Instance()->Load(stream);
}

//------------------------------------------------------------------------------
//
void
	gosFX::EffectLibrary::Save(Stuff::MemoryStream* stream)
{
	mc2::particles::SpecLibrary::Instance()->Save(stream);
}

//------------------------------------------------------------------------------
//
gosFX::Effect::Specification*
	gosFX::EffectLibrary::Find(const char* name)
{
	return mc2::particles::SpecLibrary::Instance()->Find(name);
}

//------------------------------------------------------------------------------
//
gosFX::Effect*
	gosFX::EffectLibrary::MakeEffect(
		unsigned index,
		unsigned flags
	)
{
	gosFX::Effect::Specification *spec =
		mc2::particles::SpecLibrary::Instance()->At(index);
	Check_Object(spec);

	// B1 Stage 2' C7-revised: under MC2_GPU_PARTICLES=1, return a
	// particles-backed Effect shell whose Start() routes to
	// mc2::particles::Spawn(). Producers see an unchanged gosFX::Effect
	// API surface; the legacy primitive subclass is never constructed.
	// When MC2_GPU_PARTICLES=0 (default during B1) the legacy branch below
	// runs and behavior is byte-identical to pre-C7-revised.
	if (mc2::particles::Batcher::is_enabled()) {
		gos_PushCurrentHeap(gosFX::Heap);
		gosFX::Effect *adapter = new mc2::particles::EffectAdapter(spec, flags);
		gos_PopCurrentHeap();
		return adapter;
	}

	gosFX::Effect::ClassData *data =
		Cast_Pointer(
			gosFX::Effect::ClassData*,
			Stuff::RegisteredClass::FindClassData(spec->GetClassID())
		);
	Check_Object(data);
	Check_Pointer(data->effectFactory);
	return (*data->effectFactory)(spec, flags);
}
