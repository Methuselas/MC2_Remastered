/***************************************************************
* FILENAME: ObjectiveDlg.cpp
* DESCRIPTION: Implements the Editor objective dialog.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Updated Editor Remaster comments and attribution header.
****************************************************************/

// ObjectiveDlg.cpp : implementation file
//

#include "stdafx.h"
#include "resource.h"
#include "ObjectiveDlg.h"

#include "assert.h"
#include "EditorInterface.h"
#include "ResourceStringSelectionDlg.h"
#include "UserTextEdit.h"
#include "ViewConditionOrActionDlg.h"
#include "EditorResourceFallback.h"

#if 0 /*gos doesn't like this */
#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif
#endif /*gos doesn't like this */

/////////////////////////////////////////////////////////////////////////////
// ObjectiveDlg dialog


ObjectiveDlg::ObjectiveDlg(CWnd* pParent /*=NULL*/)
	: CDialog(ObjectiveDlg::IDD, pParent)
{
	m_TitleResourceStringID = 0;
	m_DescriptionResourceStringID = 0;

	//{{AFX_DATA_INIT(ObjectiveDlg)
	m_PriorityRadioButton = -1;
	m_ResourcePointsEdit = 0;
	m_PreviousPrimaryObjectiveMustBeCompleteCheckButton = FALSE;
	m_AllPreviousPrimaryObjectivesMustBeCompleteCheckButton = FALSE;
	m_DisplayMarkerCheckButton = FALSE;
	m_XEdit = 0.0f;
	m_YEdit = 0.0f;
	m_HiddenTriggerCheckButton = FALSE;
	m_ActivateOnFlagCheckButton = FALSE;
	m_TitleEdit = _T("");
	m_DescriptionEdit = _T("");
	m_TitleResourceStringIDEdit = _T("");
	m_DescriptionResourceStringIDEdit = _T("");
	m_ActivateFlagIDEdit = _T("");
	m_ResetStatusOnFlagCheckButton = FALSE;
	m_ResetStatusFlagIDEdit = _T("");
	//}}AFX_DATA_INIT

	nConditionSpeciesSelectionIndex = -1;
	nActionSpeciesSelectionIndex = -1;
	nFailureConditionSpeciesSelectionIndex = -1;
	nFailureActionSpeciesSelectionIndex = -1;
}


void ObjectiveDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	//{{AFX_DATA_MAP(ObjectiveDlg)
	DDX_Control(pDX, IDC_OBJECTIVE_ADD_FAILURE_CONDITION_BUTTON, m_AddFailureConditionButton);
	DDX_Control(pDX, IDC_OBJECTIVE_FAILURE_ACTION_LIST, m_FailureActionList);
	DDX_Control(pDX, IDC_OBJECTIVE_FAILURE_CONDITION_LIST, m_FailureConditionList);
	DDX_Control(pDX, IDC_OBJECTIVE_TYPE_OF_FAILURE_ACTION_COMBOBOX, m_FailureActionComboBox);
	DDX_Control(pDX, IDC_OBJECTIVE_TYPE_OF_FAILURE_CONDITION_COMBOBOX, m_FailureConditionComboBox);
	DDX_Control(pDX, IDC_OBJECTIVEGROUP, m_modelGroup);
	DDX_Control(pDX, IDC_MECH2, m_Mech);
	DDX_Control(pDX, IDC_OBJECTIVE_ADD_CONDITION_BUTTON, m_AddButton);
	DDX_Control(pDX, IDC_OBJECTIVE_TYPE_OF_ACTION_COMBOBOX, m_ActionComboBox);
	DDX_Control(pDX, IDC_OBJECTIVE_TYPE_OF_CONDITION_COMBOBOX, m_ComboBox);
	DDX_Control(pDX, IDC_OBJECTIVE_ACTION_LIST, m_ActionList);
	DDX_Control(pDX, IDC_OBJECTIVE_CONDITION_LIST, m_List);
	DDX_Radio(pDX, IDC_OBJECTIVE_PRIMARY_RADIO, m_PriorityRadioButton);
	DDX_Text(pDX, IDC_OBJECTIVE_RESOURCE_POINTS_EDITBOX, m_ResourcePointsEdit);
	DDX_Check(pDX, IDC_OBJECTIVE_PREVIOUS_CHECK, m_PreviousPrimaryObjectiveMustBeCompleteCheckButton);
	DDX_Check(pDX, IDC_OBJECTIVE_ALL_PREVIOUS_CHECK, m_AllPreviousPrimaryObjectivesMustBeCompleteCheckButton);
	DDX_Check(pDX, IDC_OBJECTIVE_DISPLAY_MARKER_CHECK, m_DisplayMarkerCheckButton);
	DDX_Text(pDX, IDC_OBJECTIVE_X_EDITBOX, m_XEdit);
	DDX_Text(pDX, IDC_OBJECTIVE_Y_EDITBOX, m_YEdit);
	DDX_Check(pDX, IDC_OBJECTIVE_HIDDEN_TRIGGER_CHECK, m_HiddenTriggerCheckButton);
	DDX_Check(pDX, IDC_OBJECTIVE_ACTIVATE_ON_FLAG_CHECK, m_ActivateOnFlagCheckButton);
	DDX_Text(pDX, IDC_OBJECTIVE_TITLE_EDIT, m_TitleEdit);
	DDX_Text(pDX, IDC_OBJECTIVE_DESCRIPTION_EDIT, m_DescriptionEdit);
	DDX_Text(pDX, IDC_OBJECTIVE_TITLE_RESOURCE_STRING_ID_EDIT, m_TitleResourceStringIDEdit);
	DDX_Text(pDX, IDC_OBJECTIVE_DESCRIPTION_RESOURCE_STRING_ID_EDIT, m_DescriptionResourceStringIDEdit);
	DDX_Text(pDX, IDC_OBJECTIVE_ACTIVATE_FLAG_EDITBOX, m_ActivateFlagIDEdit);
	DDX_Check(pDX, IDC_OBJECTIVE_RESET_STATUS_ON_FLAG_CHECK, m_ResetStatusOnFlagCheckButton);
	DDX_Text(pDX, IDC_OBJECTIVE_RESET_STATUS_FLAG_EDITBOX, m_ResetStatusFlagIDEdit);
	//}}AFX_DATA_MAP
}


BEGIN_MESSAGE_MAP(ObjectiveDlg, CDialog)
	//{{AFX_MSG_MAP(ObjectiveDlg)
	ON_BN_CLICKED(IDC_OBJECTIVE_ADD_CONDITION_BUTTON, OnObjectiveAddConditionButton)
	ON_BN_CLICKED(IDC_OBJECTIVE_REMOVE_CONDITION_BUTTON, OnObjectiveRemoveConditionButton)
	ON_BN_CLICKED(IDC_OBJECTIVE_ADD_ACTION_BUTTON, OnObjectiveAddActionButton)
	ON_BN_CLICKED(IDC_OBJECTIVE_REMOVE_ACTION_BUTTON, OnObjectiveRemoveActionButton)
	ON_CBN_SELCHANGE(IDC_OBJECTIVEGROUP, OnSelchangeGroup)
	ON_BN_CLICKED(IDC_BASEEDIT2, OnBaseedit2)
	ON_BN_CLICKED(IDC_HIGHILIGHT2EDIT2, OnHighilight2edit2)
	ON_BN_CLICKED(IDC_HIGHLIGHT1EDIT, OnHighlight1edit)
	ON_EN_CHANGE(IDC_HIGHLIGHT1, OnChangeHighlight1)
	ON_EN_CHANGE(IDC_HIGHLIGHT2, OnChangeHighlight2)
	ON_EN_CHANGE(IDC_BASE2, OnChangeBase2)
	ON_WM_CTLCOLOR()
	ON_BN_CLICKED(IDC_OBJECTIVE_TITLE_EDIT_BUTTON, OnObjectiveTitleEditButton)
	ON_BN_CLICKED(IDC_OBJECTIVE_DESCRIPTION_EDIT_BUTTON, OnObjectiveDescriptionEditButton)
	ON_BN_CLICKED(IDC_OBJECTIVE_ADD_FAILURE_CONDITION_BUTTON, OnObjectiveAddFailureConditionButton)
	ON_BN_CLICKED(IDC_OBJECTIVE_REMOVE_FAILURE_CONDITION_BUTTON, OnObjectiveRemoveFailureConditionButton)
	ON_BN_CLICKED(IDC_OBJECTIVE_ADD_FAILURE_ACTION_BUTTON, OnObjectiveAddFailureActionButton)
	ON_BN_CLICKED(IDC_OBJECTIVE_REMOVE_FAILURE_ACTION_BUTTON, OnObjectiveRemoveFailureActionButton)
	ON_BN_CLICKED(IDC_OBJECTIVE_VIEW_CONDITION_BUTTON, OnObjectiveViewConditionButton)
	ON_BN_CLICKED(IDC_OBJECTIVE_VIEW_ACTION_BUTTON, OnObjectiveViewActionButton)
	ON_BN_CLICKED(IDC_OBJECTIVE_VIEW_FAILURE_CONDITION_BUTTON, OnObjectiveViewFailureConditionButton)
	ON_BN_CLICKED(IDC_OBJECTIVE_VIEW_FAILURE_ACTION_BUTTON, OnObjectiveViewFailureActionButton)
	ON_BN_CLICKED(IDC_OBJECTIVE_CLEAR_LOGISTICS_MODEL_BUTTON, OnObjectiveClearLogisticsModelButton)
	//}}AFX_MSG_MAP
	ON_BN_CLICKED(IDC_OBJECTIVE_PICK_MARKER_BUTTON, OnObjectivePickMarkerButton)
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// ObjectiveDlg message handlers

// --- Readable WHEN/THEN line formatting -------------------------------------
// Build a plain-English sentence for one condition/action, keyed on Species().
// Replaces the old "TypeLabel  rawParams" dump. Object-ref species (specific
// unit/structure) read coordinates from the SAFE noted-position accessors
// (NotedX/NotedY) and NEVER deref the live object pointer -- the engine
// InstanceDescription() for those species derefs m_pUnit/m_pBuilding unguarded
// and asserts the display name, which is unsafe from the editor list.
EString objReadableCondition(CObjectiveCondition *cond) {
	EString out;
	if (!cond) { out = "(empty condition)"; return out; }

	switch (cond->Species())
	{
	case DESTROY_ALL_ENEMY_UNITS:               out = "Destroy all enemy units"; break;
	case CAPTURE_OR_DESTROY_ALL_ENEMY_UNITS:    out = "Capture or destroy all enemy units"; break;
	case DEAD_OR_FLED_ALL_ENEMY_UNITS:          out = "All enemy units dead or fled"; break;
	case DESTROY_ENEMY_UNIT_GROUP:              out = "Destroy an enemy unit group"; break;
	case CAPTURE_OR_DESTROY_ENEMY_UNIT_GROUP:   out = "Capture or destroy an enemy unit group"; break;
	case DEAD_OR_FLED_ENEMY_UNIT_GROUP:         out = "An enemy unit group is dead or fled"; break;

	case DESTROY_NUMBER_OF_ENEMY_UNITS:
	case CAPTURE_OR_DESTROY_NUMBER_OF_ENEMY_UNITS:
	case DEAD_OR_FLED_NUMBER_OF_ENEMY_UNITS:
	{
		int n = 0;
		if (CNumberOfEnemyUnitsObjectiveCondition *p =
				dynamic_cast<CNumberOfEnemyUnitsObjectiveCondition *>(cond))
			n = p->Num();
		const char *verb =
			(cond->Species() == DESTROY_NUMBER_OF_ENEMY_UNITS) ? "Destroy" :
			(cond->Species() == CAPTURE_OR_DESTROY_NUMBER_OF_ENEMY_UNITS) ? "Capture or destroy" :
			"Disable (dead or fled)";
		out.Format("%s %d enemy unit%s", verb, n, (n == 1) ? "" : "s");
		break;
	}

	case DESTROY_SPECIFIC_ENEMY_UNIT:
	case CAPTURE_OR_DESTROY_SPECIFIC_ENEMY_UNIT:
	case DEAD_OR_FLED_SPECIFIC_ENEMY_UNIT:
	case CAPTURE_UNIT:
	case GUARD_SPECIFIC_UNIT:
	{
		EString tgt;
		if (CSpecificUnitObjectiveCondition *p =
				dynamic_cast<CSpecificUnitObjectiveCondition *>(cond))
			tgt = p->ReadableTarget();
		else
			tgt = "(unresolved)";
		const char *verb =
			(cond->Species() == DESTROY_SPECIFIC_ENEMY_UNIT) ? "Destroy" :
			(cond->Species() == CAPTURE_OR_DESTROY_SPECIFIC_ENEMY_UNIT) ? "Capture or destroy" :
			(cond->Species() == DEAD_OR_FLED_SPECIFIC_ENEMY_UNIT) ? "Disable (dead or fled)" :
			(cond->Species() == CAPTURE_UNIT) ? "Capture" : "Guard";
		const char *noun = (cond->Species() == GUARD_SPECIFIC_UNIT) ? "friendly unit" : "enemy unit";
		out.Format("%s %s: %s", verb, noun, tgt.Data());
		break;
	}

	case DESTROY_SPECIFIC_STRUCTURE:
	case CAPTURE_OR_DESTROY_SPECIFIC_STRUCTURE:
	case CAPTURE_STRUCTURE:
	case GUARD_SPECIFIC_STRUCTURE:
	{
		EString tgt;
		if (CSpecificStructureObjectiveCondition *p =
				dynamic_cast<CSpecificStructureObjectiveCondition *>(cond))
			tgt = p->ReadableTarget();
		else
			tgt = "(unresolved)";
		const char *verb =
			(cond->Species() == DESTROY_SPECIFIC_STRUCTURE) ? "Destroy" :
			(cond->Species() == CAPTURE_OR_DESTROY_SPECIFIC_STRUCTURE) ? "Capture or destroy" :
			(cond->Species() == CAPTURE_STRUCTURE) ? "Capture" : "Guard";
		const char *noun = (cond->Species() == GUARD_SPECIFIC_STRUCTURE) ? "friendly structure" : "structure";
		out.Format("%s %s: %s", verb, noun, tgt.Data());
		break;
	}

	case MOVE_ANY_UNIT_TO_AREA:
	case MOVE_ALL_UNITS_TO_AREA:
	case MOVE_ALL_SURVIVING_UNITS_TO_AREA:
	case MOVE_ALL_SURVIVING_MECHS_TO_AREA:
	{
		float x = 0.f, y = 0.f, r = 0.f;
		if (CAreaObjectiveCondition *p = dynamic_cast<CAreaObjectiveCondition *>(cond))
		{ x = p->TargetCenterX(); y = p->TargetCenterY(); r = p->TargetRadius(); }
		const char *who =
			(cond->Species() == MOVE_ANY_UNIT_TO_AREA) ? "any unit" :
			(cond->Species() == MOVE_ALL_UNITS_TO_AREA) ? "all units" :
			(cond->Species() == MOVE_ALL_SURVIVING_UNITS_TO_AREA) ? "all surviving units" :
			"all surviving 'Mechs";
		out.Format("Move %s into area (%.0f, %.0f) radius %.0f", who, x, y, r);
		break;
	}

	case BOOLEAN_FLAG_IS_SET:
	{
		const char *flag = "flag"; bool val = true;
		if (CBooleanFlagIsSet *p = dynamic_cast<CBooleanFlagIsSet *>(cond))
		{ const char *f = p->FlagID(); if (f && f[0]) flag = f; val = p->FlagValue(); }
		out.Format("When flag '%s' is %s", flag, val ? "set" : "clear");
		break;
	}

	case ELAPSED_MISSION_TIME:
	{
		float t = 0.f;
		if (CElapsedMissionTime *p = dynamic_cast<CElapsedMissionTime *>(cond))
			t = p->Time();
		out.Format("After %.0f seconds of mission time", t);
		break;
	}

	default:
		out = cond->Description();   // unknown species -> engine label fallback
		break;
	}
	return out;
}

EString objReadableAction(CObjectiveAction *act) {
	EString out;
	if (!act) { out = "(empty action)"; return out; }

	switch (act->Species())
	{
	case PLAY_BIK:
	{
		const char *p = "(movie)";
		if (CPlayBIK *a = dynamic_cast<CPlayBIK *>(act)) { const char *s = a->Pathname(); if (s && s[0]) p = s; }
		out.Format("Play movie %s", p);
		break;
	}
	case PLAY_WAV:
	{
		const char *p = "(sound)";
		if (CPlayWAV *a = dynamic_cast<CPlayWAV *>(act)) { const char *s = a->Pathname(); if (s && s[0]) p = s; }
		out.Format("Play sound %s", p);
		break;
	}
	case DISPLAY_TEXT_MESSAGE:
	{
		const char *m = "";
		if (CDisplayTextMessage *a = dynamic_cast<CDisplayTextMessage *>(act)) { const char *s = a->Message(); if (s) m = s; }
		out.Format("Show message: \"%s\"", m);
		break;
	}
	case DISPLAY_RESOURCE_TEXT_MESSAGE:
	{
		int id = 0;
		if (CDisplayResourceTextMessage *a = dynamic_cast<CDisplayResourceTextMessage *>(act)) id = a->ResourceStringID();
		out.Format("Show message (resource string #%d)", id);
		break;
	}
	case SET_BOOLEAN_FLAG:
	{
		const char *flag = "flag"; bool val = true;
		if (CSetBooleanFlag *a = dynamic_cast<CSetBooleanFlag *>(act)) { const char *f = a->FlagID(); if (f && f[0]) flag = f; val = a->FlagValue(); }
		out.Format("Set flag '%s' to %s", flag, val ? "set" : "clear");
		break;
	}
	case MAKE_NEW_TECHNOLOGY_AVAILABLE:
	{
		const char *p = "(file)";
		if (CMakeNewTechnologyAvailable *a = dynamic_cast<CMakeNewTechnologyAvailable *>(act)) { const char *s = a->PurchaseFile(); if (s && s[0]) p = s; }
		out.Format("Unlock technology from %s", p);
		break;
	}
	default:
		out = act->Description();
		break;
	}
	return out;
}

static void syncConditionsListWithListBox(const CObjective::condition_list_type *pConditionList, CListBox *pList) {
	pList->ResetContent();
	EString tmpStr;
	CObjective::condition_list_type::EConstIterator it = pConditionList->Begin();
	while (!it.IsDone())
	{
		tmpStr = objReadableCondition(*it);
		pList->AddString(tmpStr.Data());
		it++;
	}
}

static void syncActionsListWithListBox(const CObjective::action_list_type *pActionList, CListBox *pList) {
	pList->ResetContent();
	EString tmpStr;
	CObjective::action_list_type::EConstIterator it = pActionList->Begin();
	while (!it.IsDone())
	{
		tmpStr = objReadableAction(*it);
		pList->AddString(tmpStr.Data());
		it++;
	}
}

static BOOL CSLoadString(int resourceID, CString &targetStr) {
	char szTmp[16384/*max string length*/];
	EditorSafeLoadString( resourceID, szTmp, 16384/*max string length*/ );
	targetStr = szTmp;
	CString tmpStr;
	tmpStr.Format("mc2res.dll:%d Not defined", resourceID);
	if (0 == strcmp(tmpStr.GetBuffer(0), szTmp)) {
		return (0);
	}
	return (!0);
}

void ObjectiveDlg::UpdateTitleDisplay() {
	UpdateData(TRUE);
	if (m_TitleUseResourceString) {
		m_TitleResourceStringIDEdit.Format("%d", m_TitleResourceStringID);
		int ret = CSLoadString(m_TitleResourceStringID, m_TitleEdit);
		if (0 == ret) {
			m_TitleEdit = _TEXT("");
		}
	} else {
		m_TitleResourceStringIDEdit = _TEXT("");
		m_TitleEdit = m_TitleUnlocalizedText;
	}
	UpdateData(FALSE);
}

void ObjectiveDlg::UpdateDescriptionDisplay() {
	UpdateData(TRUE);
	if (m_DescriptionUseResourceString) {
		m_DescriptionResourceStringIDEdit.Format("%d", m_DescriptionResourceStringID);
		int ret = CSLoadString(m_DescriptionResourceStringID, m_DescriptionEdit);
		if (0 == ret) {
			m_DescriptionEdit = _TEXT("");
		}
	} else {
		m_DescriptionResourceStringIDEdit = _TEXT("");
		m_DescriptionEdit = m_DescriptionUnlocalizedText;
	}
	UpdateData(FALSE);
}

void ObjectiveDlg::LoadDialogValues() {
	(&m_ComboBox)->SetCurSel(nConditionSpeciesSelectionIndex);
	(&m_ActionComboBox)->SetCurSel(nActionSpeciesSelectionIndex);
	(&m_FailureConditionComboBox)->SetCurSel(nFailureConditionSpeciesSelectionIndex);
	(&m_FailureActionComboBox)->SetCurSel(nFailureActionSpeciesSelectionIndex);

	m_TitleUnlocalizedText = m_ModifiedObjective.Title().Data();
	m_TitleUseResourceString = m_ModifiedObjective.TitleUseResourceString();
	m_TitleResourceStringID = m_ModifiedObjective.TitleResourceStringID();
	UpdateTitleDisplay();

	m_DescriptionUnlocalizedText = m_ModifiedObjective.Description().Data();
	m_DescriptionUseResourceString = m_ModifiedObjective.DescriptionUseResourceString();
	m_DescriptionResourceStringID = m_ModifiedObjective.DescriptionResourceStringID();
	UpdateDescriptionDisplay();

	if (1 != m_ModifiedObjective.Priority()) {
		m_PriorityRadioButton = 1;
	} else {
		m_PriorityRadioButton = 0;
	}

	if (m_ModifiedObjective.PreviousPrimaryObjectiveMustBeComplete()) {
		m_PreviousPrimaryObjectiveMustBeCompleteCheckButton = TRUE;
	} else {
		m_PreviousPrimaryObjectiveMustBeCompleteCheckButton = FALSE;
	}

	if (m_ModifiedObjective.AllPreviousPrimaryObjectivesMustBeComplete()) {
		m_AllPreviousPrimaryObjectivesMustBeCompleteCheckButton = TRUE;
	} else {
		m_AllPreviousPrimaryObjectivesMustBeCompleteCheckButton = FALSE;
	}

	m_ResourcePointsEdit = m_ModifiedObjective.ResourcePoints();

	if (m_ModifiedObjective.DisplayMarker()) {
		m_DisplayMarkerCheckButton = TRUE;
	} else {
		m_DisplayMarkerCheckButton = FALSE;
	}

	m_XEdit = m_ModifiedObjective.MarkerX();
	m_YEdit = m_ModifiedObjective.MarkerY();

	if (m_ModifiedObjective.IsHiddenTrigger()) {
		m_HiddenTriggerCheckButton = TRUE;
	} else {
		m_HiddenTriggerCheckButton = FALSE;
	}

	if (m_ModifiedObjective.ActivateOnFlag()) {
		m_ActivateOnFlagCheckButton = TRUE;
	} else {
		m_ActivateOnFlagCheckButton = FALSE;
	}
	m_ActivateFlagIDEdit = m_ModifiedObjective.ActivateFlagID().Data();

	if (m_ModifiedObjective.ResetStatusOnFlag()) {
		m_ResetStatusOnFlagCheckButton = TRUE;
	} else {
		m_ResetStatusOnFlagCheckButton = FALSE;
	}
	m_ResetStatusFlagIDEdit = m_ModifiedObjective.ResetStatusFlagID().Data();

	syncConditionsListWithListBox((&m_ModifiedObjective), (&m_List));
	syncActionsListWithListBox(&(m_ModifiedObjective.m_actionList), (&m_ActionList));
	syncConditionsListWithListBox(&(m_ModifiedObjective.m_failureConditionList), (&m_FailureConditionList));
	syncActionsListWithListBox(&(m_ModifiedObjective.m_failureActionList), (&m_FailureActionList));

	int color = m_ModifiedObjective.BaseColor();
	char tmp[256];
	sprintf( tmp, "0x%x", color );
	GetDlgItem( IDC_BASE2 )->SetWindowText( tmp );

	color = m_ModifiedObjective.HighlightColor();
	sprintf( tmp, "0x%x", color );
	GetDlgItem( IDC_HIGHLIGHT1 )->SetWindowText( tmp );

	color = m_ModifiedObjective.HighlightColor2();
	sprintf( tmp, "0x%x", color );
	GetDlgItem( IDC_HIGHLIGHT2 )->SetWindowText( tmp );

	int modelID = m_ModifiedObjective.ModelID();

	if ( modelID != -1 )
	{
		unsigned long ulGroup, ulIndex;
		EditorObjectMgr::instance()->getBuildingFromID( modelID, ulGroup, ulIndex, true);
		m_modelGroup.SetCurSel( ulGroup );
		OnSelchangeGroup();
		m_Mech.SetCurSel( ulIndex );
	}

	UpdateData(FALSE);
}

void ObjectiveDlg::SaveDialogValues() {
	UpdateData(TRUE);

	m_ModifiedObjective.Title(m_TitleUnlocalizedText.GetBuffer(0));
	m_ModifiedObjective.TitleUseResourceString(TRUE == m_TitleUseResourceString);
	m_ModifiedObjective.TitleResourceStringID(m_TitleResourceStringID);

	m_ModifiedObjective.Description(m_DescriptionUnlocalizedText.GetBuffer(0));
	m_ModifiedObjective.DescriptionUseResourceString(TRUE == m_DescriptionUseResourceString);
	m_ModifiedObjective.DescriptionResourceStringID(m_DescriptionResourceStringID);

	if (0 == m_PriorityRadioButton) {
		m_ModifiedObjective.Priority(1);
	} else {
		m_ModifiedObjective.Priority(2);
	}

	m_ModifiedObjective.ResourcePoints(m_ResourcePointsEdit);

	if (m_PreviousPrimaryObjectiveMustBeCompleteCheckButton) {
		m_ModifiedObjective.PreviousPrimaryObjectiveMustBeComplete(true);
	} else {
		m_ModifiedObjective.PreviousPrimaryObjectiveMustBeComplete(false);
	}

	if (m_AllPreviousPrimaryObjectivesMustBeCompleteCheckButton) {
		m_ModifiedObjective.AllPreviousPrimaryObjectivesMustBeComplete(true);
	} else {
		m_ModifiedObjective.AllPreviousPrimaryObjectivesMustBeComplete(false);
	}

	if (m_DisplayMarkerCheckButton) {
		m_ModifiedObjective.DisplayMarker(true);
	} else {
		m_ModifiedObjective.DisplayMarker(false);
	}

	m_ModifiedObjective.MarkerX(m_XEdit);
	m_ModifiedObjective.MarkerY(m_YEdit);

	if (m_HiddenTriggerCheckButton) {
		m_ModifiedObjective.IsHiddenTrigger(true);
	} else {
		m_ModifiedObjective.IsHiddenTrigger(false);
	}

	if (m_ActivateOnFlagCheckButton) {
		m_ModifiedObjective.ActivateOnFlag(true);
	} else {
		m_ModifiedObjective.ActivateOnFlag(false);
	}
	m_ModifiedObjective.ActivateFlagID(m_ActivateFlagIDEdit.GetBuffer(0));

	if (m_ResetStatusOnFlagCheckButton) {
		m_ModifiedObjective.ResetStatusOnFlag(true);
	} else {
		m_ModifiedObjective.ResetStatusOnFlag(false);
	}
	m_ModifiedObjective.ResetStatusFlagID(m_ResetStatusFlagIDEdit.GetBuffer(0));

	long base, highlight1, highlight2;

	CWnd* pWnd = GetDlgItem( IDC_BASE2 );
	if ( pWnd )
	{
		CString tmpStr;
		pWnd->GetWindowText( tmpStr );

		if ( tmpStr.GetLength() )
		{
			tmpStr.Replace( "0x", "" );
			sscanf( tmpStr, "%x", &base );
			base |= 0xff000000;
			m_ModifiedObjective.BaseColor( base );
		}
		
	}

	pWnd = GetDlgItem( IDC_HIGHLIGHT1 );
	if ( pWnd )
	{
		CString tmpStr;
		pWnd->GetWindowText( tmpStr );

		if ( tmpStr.GetLength() )
		{
			tmpStr.Replace( "0x", "" );
			sscanf( tmpStr, "%x", &highlight1 );
			highlight1 |= 0xff000000;
			m_ModifiedObjective.HighlightColor( highlight1 );
		}
		
	}

	pWnd = GetDlgItem( IDC_HIGHLIGHT2 );
	if ( pWnd )
	{
		CString tmpStr;
		pWnd->GetWindowText( tmpStr );

		if ( tmpStr.GetLength() )
		{
			tmpStr.Replace( "0x", "" );
			sscanf( tmpStr, "%x", &highlight2 );
			highlight2 |= 0xff000000;
			m_ModifiedObjective.HighlightColor2( highlight2 );
		}
		
	}

	int group = m_modelGroup.GetCurSel();
	int index = m_Mech.GetCurSel();

	if ( group != -1  && index != -1 )
	{

		int ID = EditorObjectMgr::instance()->getID( group, index );
		int fitID = EditorObjectMgr::instance()->getFitID(ID);

		m_ModifiedObjective.ModelID( fitID );
	}
	else 
		m_ModifiedObjective.ModelID( -1 );
	
}

BOOL ObjectiveDlg::OnInitDialog() 
{
	CDialog::OnInitDialog();
	EditorObjectMgr* pMgr = EditorObjectMgr::instance();
	
	int groupCount = pMgr->getBuildingGroupCount();

	const char** pGroups = new const char*[groupCount];
		
	pMgr->getBuildingGroupNames(pGroups, groupCount);


	for ( int i = 0; i < groupCount; ++i )
	{
		m_modelGroup.AddString( pGroups[i] );
		m_modelGroup.SetItemData( i, (DWORD)i );
	}

	delete [] pGroups;

	{

		{
			comboBoxItems.Clear();
			comboBoxItems.Append(DESTROY_ALL_ENEMY_UNITS);
			comboBoxItems.Append(DESTROY_NUMBER_OF_ENEMY_UNITS);
			comboBoxItems.Append(DESTROY_SPECIFIC_ENEMY_UNIT);
			comboBoxItems.Append(DESTROY_SPECIFIC_STRUCTURE);
			comboBoxItems.Append(CAPTURE_OR_DESTROY_ALL_ENEMY_UNITS);
			comboBoxItems.Append(CAPTURE_OR_DESTROY_NUMBER_OF_ENEMY_UNITS);
			comboBoxItems.Append(CAPTURE_OR_DESTROY_SPECIFIC_ENEMY_UNIT);
			comboBoxItems.Append(CAPTURE_OR_DESTROY_SPECIFIC_STRUCTURE);
			comboBoxItems.Append(DEAD_OR_FLED_ALL_ENEMY_UNITS);
			comboBoxItems.Append(DEAD_OR_FLED_NUMBER_OF_ENEMY_UNITS);
			comboBoxItems.Append(DEAD_OR_FLED_SPECIFIC_ENEMY_UNIT);
			comboBoxItems.Append(CAPTURE_UNIT);
			comboBoxItems.Append(CAPTURE_STRUCTURE);
			comboBoxItems.Append(GUARD_SPECIFIC_UNIT);
			comboBoxItems.Append(GUARD_SPECIFIC_STRUCTURE);
			comboBoxItems.Append(MOVE_ANY_UNIT_TO_AREA);
			comboBoxItems.Append(MOVE_ALL_UNITS_TO_AREA);
			comboBoxItems.Append(MOVE_ALL_SURVIVING_UNITS_TO_AREA);
			comboBoxItems.Append(MOVE_ALL_SURVIVING_MECHS_TO_AREA);
			comboBoxItems.Append(BOOLEAN_FLAG_IS_SET);
			comboBoxItems.Append(ELAPSED_MISSION_TIME);
			EString tmpEStr;
			CConditionSpeciesList::EConstIterator it = comboBoxItems.Begin();
			while (!it.IsDone()) {
				tmpEStr = m_ModifiedObjective.DescriptionOfConditionSpecies(*it);
				(&m_ComboBox)->AddString(tmpEStr.Data());
				it++;
			}
			(&m_ComboBox)->SetCurSel(0);
		}

		{
			actionComboBoxItems.Clear();
			actionComboBoxItems.Append(PLAY_BIK);
			actionComboBoxItems.Append(PLAY_WAV);
			actionComboBoxItems.Append(DISPLAY_TEXT_MESSAGE);
			actionComboBoxItems.Append(DISPLAY_RESOURCE_TEXT_MESSAGE);
			actionComboBoxItems.Append(SET_BOOLEAN_FLAG);
			actionComboBoxItems.Append(MAKE_NEW_TECHNOLOGY_AVAILABLE);
			EString tmpEStr;
			CActionSpeciesList::EConstIterator it = actionComboBoxItems.Begin();
			while (!it.IsDone()) {
				tmpEStr = m_ModifiedObjective.DescriptionOfActionSpecies(*it);
				(&m_ActionComboBox)->AddString(tmpEStr.Data());
				it++;
			}
			(&m_ActionComboBox)->SetCurSel(0);
		}

		{
			failureConditionComboBoxItems.Clear();
			failureConditionComboBoxItems.Append(DESTROY_ALL_ENEMY_UNITS);
			failureConditionComboBoxItems.Append(DESTROY_NUMBER_OF_ENEMY_UNITS);
			failureConditionComboBoxItems.Append(DESTROY_SPECIFIC_ENEMY_UNIT);
			failureConditionComboBoxItems.Append(DESTROY_SPECIFIC_STRUCTURE);
			failureConditionComboBoxItems.Append(CAPTURE_OR_DESTROY_ALL_ENEMY_UNITS);
			failureConditionComboBoxItems.Append(CAPTURE_OR_DESTROY_NUMBER_OF_ENEMY_UNITS);
			failureConditionComboBoxItems.Append(CAPTURE_OR_DESTROY_SPECIFIC_ENEMY_UNIT);
			failureConditionComboBoxItems.Append(CAPTURE_OR_DESTROY_SPECIFIC_STRUCTURE);
			failureConditionComboBoxItems.Append(DEAD_OR_FLED_ALL_ENEMY_UNITS);
			failureConditionComboBoxItems.Append(DEAD_OR_FLED_NUMBER_OF_ENEMY_UNITS);
			failureConditionComboBoxItems.Append(DEAD_OR_FLED_SPECIFIC_ENEMY_UNIT);
			failureConditionComboBoxItems.Append(CAPTURE_UNIT);
			failureConditionComboBoxItems.Append(CAPTURE_STRUCTURE);
			failureConditionComboBoxItems.Append(GUARD_SPECIFIC_UNIT);
			failureConditionComboBoxItems.Append(GUARD_SPECIFIC_STRUCTURE);
			failureConditionComboBoxItems.Append(MOVE_ANY_UNIT_TO_AREA);
			failureConditionComboBoxItems.Append(MOVE_ALL_UNITS_TO_AREA);
			failureConditionComboBoxItems.Append(MOVE_ALL_SURVIVING_UNITS_TO_AREA);
			failureConditionComboBoxItems.Append(MOVE_ALL_SURVIVING_MECHS_TO_AREA);
			failureConditionComboBoxItems.Append(BOOLEAN_FLAG_IS_SET);
			failureConditionComboBoxItems.Append(ELAPSED_MISSION_TIME);
			EString tmpEStr;
			CConditionSpeciesList::EConstIterator it = failureConditionComboBoxItems.Begin();
			while (!it.IsDone()) {
				tmpEStr = m_ModifiedObjective.DescriptionOfConditionSpecies(*it);
				(&m_FailureConditionComboBox)->AddString(tmpEStr.Data());
				it++;
			}
			(&m_FailureConditionComboBox)->SetCurSel(0);
		}

		{
			failureActionComboBoxItems.Clear();
			failureActionComboBoxItems.Append(PLAY_BIK);
			failureActionComboBoxItems.Append(PLAY_WAV);
			failureActionComboBoxItems.Append(DISPLAY_TEXT_MESSAGE);
			failureActionComboBoxItems.Append(DISPLAY_RESOURCE_TEXT_MESSAGE);
			failureActionComboBoxItems.Append(SET_BOOLEAN_FLAG);
			failureActionComboBoxItems.Append(MAKE_NEW_TECHNOLOGY_AVAILABLE);
			EString tmpEStr;
			CActionSpeciesList::EConstIterator it = failureActionComboBoxItems.Begin();
			while (!it.IsDone()) {
				tmpEStr = m_ModifiedObjective.DescriptionOfActionSpecies(*it);
				(&m_FailureActionComboBox)->AddString(tmpEStr.Data());
				it++;
			}
			(&m_FailureActionComboBox)->SetCurSel(0);
		}
	}

	LoadDialogValues();

	// Slice 1 -- Marker XY map-picker result delivery. If we just returned from a
	// terrain point pick (handleLeftButtonDown set pendingPickResultReady), write
	// the captured world XY into the marker fields, settle the editor out of the
	// object-select handoff, and stop -- do NOT fall through to the condition-pick
	// re-post below (this re-entry is for the marker pick, not a condition pick).
	{
		EditorInterface *pEditor = EditorInterface::instance();
		if ((0 != pEditor) && pEditor->objectivesEditState.pendingPickResultReady) {
			m_XEdit = pEditor->objectivesEditState.pendingPickX;
			m_YEdit = pEditor->objectivesEditState.pendingPickY;
			m_ModifiedObjective.MarkerX(m_XEdit);
			m_ModifiedObjective.MarkerY(m_YEdit);
			// Picking a point implies the user wants the marker shown -- turn the
			// Display Marker checkbox on (both the objective field and the DDX member
			// bound to IDC_OBJECTIVE_DISPLAY_MARKER_CHECK) before UpdateData pushes it.
			m_ModifiedObjective.DisplayMarker(true);
			m_DisplayMarkerCheckButton = TRUE;
			UpdateData(FALSE);

			// One-shot: clear the result flag and leave object-select-only mode so
			// the normal modal accept/cancel flow resumes from here.
			pEditor->objectivesEditState.pendingPickResultReady = false;
			pEditor->objectivesEditState.pendingPickPoint = false;
			pEditor->ObjectSelectOnlyMode(false);
			return TRUE;
		}
	}

	// Marker-pick was armed but no point was delivered (user cancelled/ESC before
	// clicking the map). Disarm cleanly and do NOT fall into the condition re-post.
	if (EditorInterface::instance() &&
	    EditorInterface::instance()->objectivesEditState.pendingPickPoint &&
	    !EditorInterface::instance()->objectivesEditState.pendingPickResultReady)
	{
		EditorInterface::instance()->objectivesEditState.pendingPickPoint = false;
		EditorInterface::instance()->ObjectSelectOnlyMode(false);
		// fall through to normal (non-select-only) init below
	}

	if (EditorInterface::instance()->ObjectSelectOnlyMode()) {
		/* The dialog is already initialized. */
		if (CObjectivesEditState::SUCCESS_CONDITION == EditorInterface::instance()->objectivesEditState.listID) {
			// post a message that the "Add (Success) Condition" button was pressed
			PostMessage(WM_COMMAND, MAKEWPARAM(IDC_OBJECTIVE_ADD_CONDITION_BUTTON, BN_CLICKED), (LPARAM)((&m_AddButton)->m_hWnd));
		} else {
			// post a message that the "Add (Failure) Condition" button was pressed
			PostMessage(WM_COMMAND, MAKEWPARAM(IDC_OBJECTIVE_ADD_FAILURE_CONDITION_BUTTON, BN_CLICKED), (LPARAM)((&m_AddFailureConditionButton)->m_hWnd));
		}
	} else {
		EditorInterface::instance()->objectivesEditState.ModifiedObjective = m_ModifiedObjective;
	}

	return TRUE;  // return TRUE unless you set the focus to a control
	              // EXCEPTION: OCX Property Pages should return FALSE
}

// Slice 1 -- Marker XY map-picker (PICKER-SELECT-POINT-RECON-1, Option B).
// Mirrors the ObjectSelectOnlyMode handoff used by the condition object pickers
// (OnObjectiveAddConditionButton): save the in-progress objective into the
// editor singleton, flip the editor into selection mode, then EndDialog so the
// whole nested-modal stack unwinds and the editor render loop runs. The next
// terrain left-click is consumed by EditorInterface::handleLeftButtonDown, which
// writes the picked world XY into objectivesEditState.pendingPickX/Y, sets
// pendingPickResultReady, and re-opens the objectives dialog chain (Team ->
// CObjectives::EditDialog -> ObjectivesDlg re-posts EDIT -> CObjective::EditDialog
// -> this dialog). OnInitDialog then loads the picked XY exactly once.
void ObjectiveDlg::OnObjectivePickMarkerButton()
{
	EditorInterface *pEditor = EditorInterface::instance();
	if (0 == pEditor) {
		return;
	}

	// Capture the current control state (incl. the X/Y editboxes and the marker
	// checkbox) into m_ModifiedObjective so nothing the user typed is lost.
	SaveDialogValues();

	pEditor->objectivesEditState.ModifiedObjective = m_ModifiedObjective;
	// Restore the same combo selections the ordinary object-pick handoff stashes,
	// so the re-opened dialog comes back to the same condition/action species.
	// NOTE: objectiveFunction (ADD vs EDIT) is intentionally NOT set here -- the
	// parent ObjectivesDlg::OnObjectives{Add,Edit}Button handler that opened this
	// ObjectiveDlg sets it during its own ObjectSelectOnlyMode unwind (see
	// OnObjectivesAddButton/OnObjectivesEditButton), so forcing it here would break
	// the ADD re-open path.
	pEditor->objectivesEditState.listID = CObjectivesEditState::SUCCESS_CONDITION;
	pEditor->objectivesEditState.nConditionSpeciesSelectionIndex = (&m_ComboBox)->GetCurSel();
	pEditor->objectivesEditState.nActionSpeciesSelectionIndex = (&m_ActionComboBox)->GetCurSel();
	pEditor->objectivesEditState.nFailureConditionSpeciesSelectionIndex = (&m_FailureConditionComboBox)->GetCurSel();
	pEditor->objectivesEditState.nFailureActionSpeciesSelectionIndex = (&m_FailureActionComboBox)->GetCurSel();

	// Arm the one-shot terrain point pick. Result delivery clears pendingPickPoint
	// and sets pendingPickResultReady (consumed by OnInitDialog).
	pEditor->objectivesEditState.pendingPickPoint = true;
	pEditor->objectivesEditState.pendingPickResultReady = false;

	// Put the editor into selection mode and the objective-select-only handoff
	// mode, exactly like the unit/building pickers, so the re-open machinery fires.
	pEditor->SelectionMode();
	pEditor->ObjectSelectOnlyMode(true);

	EndDialog(IDOK);
}

void ObjectiveDlg::OnObjectiveAddConditionButton()
{
	{
		DWORD comboboxSelection = (&m_ComboBox)->GetCurSel();
		if ((0 <= comboboxSelection) && (comboBoxItems.Count() > comboboxSelection)) {
			//condition_species_type species = comboBoxItems[comboboxSelection];
			condition_species_type species = *(comboBoxItems.Iterator(comboboxSelection));
			pNewCondition = CObjective::new_CObjectiveCondition(species, m_ModifiedObjective.Alignment());
		} else {
			return;
		}
	}
	assert(pNewCondition);

	/* This call may set the editor into ObjectSelectOnlyMode (i.e.: set the  value of
	EditorInterface::instance()->ObjectSelectOnlyMode() to true) */
	bool result = pNewCondition->EditDialog();

	if (EditorInterface::instance()->ObjectSelectOnlyMode()) {
		/* close the dialog and enter ObjectSelectOnlyMode */
		SaveDialogValues();
		EditorInterface::instance()->objectivesEditState.ModifiedObjective = m_ModifiedObjective;
		EditorInterface::instance()->objectivesEditState.listID = CObjectivesEditState::SUCCESS_CONDITION;
		EditorInterface::instance()->objectivesEditState.nConditionSpeciesSelectionIndex = (&m_ComboBox)->GetCurSel();
		EditorInterface::instance()->objectivesEditState.nActionSpeciesSelectionIndex = (&m_ActionComboBox)->GetCurSel();
		EditorInterface::instance()->objectivesEditState.nFailureConditionSpeciesSelectionIndex = (&m_FailureConditionComboBox)->GetCurSel();
		EditorInterface::instance()->objectivesEditState.nFailureActionSpeciesSelectionIndex = (&m_FailureActionComboBox)->GetCurSel();
		delete pNewCondition; pNewCondition = 0;
		EndDialog(IDOK);
		return;
	} else {
		if (true == result) {
			m_ModifiedObjective.Append(pNewCondition);
			m_List.SetCurSel(m_List.GetCount() - 1);
			syncConditionsListWithListBox((&m_ModifiedObjective), (&m_List));
		} else {
			delete pNewCondition; pNewCondition = 0;
		}
		return;
	}
}

void ObjectiveDlg::OnObjectiveRemoveConditionButton() 
{
	int nSelectionIndex = (&m_List)->GetCurSel();
	if ((0 <= nSelectionIndex) && (m_ModifiedObjective.Count() > nSelectionIndex)) {
		// should put up confirmation box here
		delete *(m_ModifiedObjective.Iterator(nSelectionIndex));
		m_ModifiedObjective.Delete(nSelectionIndex);
		syncConditionsListWithListBox((&m_ModifiedObjective), (&m_List));
		if (0 < m_List.GetCount()) {
			if (m_List.GetCount() <= (long)nSelectionIndex) {
				nSelectionIndex = m_List.GetCount() - 1;
			}
			m_List.SetCurSel(nSelectionIndex);
		}
	}
}

void ObjectiveDlg::OnObjectiveAddActionButton() 
{
	{
		DWORD actionComboboxSelection = (&m_ActionComboBox)->GetCurSel();
		if ((0 <= actionComboboxSelection) && (actionComboBoxItems.Count() > actionComboboxSelection)) {
			//action_species_type species = actionComboBoxItems[actionComboboxSelection];
			action_species_type species = *(actionComboBoxItems.Iterator(actionComboboxSelection));
			pNewAction = CObjective::new_CObjectiveAction(species, m_ModifiedObjective.Alignment());
		} else {
			return;
		}
	}
	assert(pNewAction);

	/* This call may set the editor into ObjectSelectOnlyMode (i.e.: set the  value of
	EditorInterface::instance()->ObjectSelectOnlyMode() to true) */
	bool result = pNewAction->EditDialog();

	if (EditorInterface::instance()->ObjectSelectOnlyMode()) {
		/* close the dialog and enter ObjectSelectOnlyMode */
		SaveDialogValues();
		EditorInterface::instance()->objectivesEditState.ModifiedObjective = m_ModifiedObjective;
		EditorInterface::instance()->objectivesEditState.nConditionSpeciesSelectionIndex = (&m_ComboBox)->GetCurSel();
		EditorInterface::instance()->objectivesEditState.nActionSpeciesSelectionIndex = (&m_ActionComboBox)->GetCurSel();
		EditorInterface::instance()->objectivesEditState.nFailureConditionSpeciesSelectionIndex = (&m_FailureConditionComboBox)->GetCurSel();
		EditorInterface::instance()->objectivesEditState.nFailureActionSpeciesSelectionIndex = (&m_FailureActionComboBox)->GetCurSel();
		delete pNewAction; pNewAction = 0;
		EndDialog(IDOK);
		return;
	} else {
		if (true == result) {
			m_ModifiedObjective.m_actionList.Append(pNewAction);
			m_ActionList.SetCurSel(m_ActionList.GetCount() - 1);
			syncActionsListWithListBox(&(m_ModifiedObjective.m_actionList), (&m_ActionList));
		} else {
			delete pNewAction; pNewAction = 0;
		}
		return;
	}
}

void ObjectiveDlg::OnObjectiveRemoveActionButton() 
{
	int nSelectionIndex = (&m_ActionList)->GetCurSel();
	if ((0 <= nSelectionIndex) && (m_ModifiedObjective.m_actionList.Count() > nSelectionIndex)) {
		// should put up confirmation box here
		delete *(m_ModifiedObjective.m_actionList.Iterator(nSelectionIndex));
		m_ModifiedObjective.m_actionList.Delete(nSelectionIndex);
		syncActionsListWithListBox(&(m_ModifiedObjective.m_actionList), (&m_ActionList));
		if (0 < m_ActionList.GetCount()) {
			if (m_ActionList.GetCount() <= (long)nSelectionIndex) {
				nSelectionIndex = m_ActionList.GetCount() - 1;
			}
			m_ActionList.SetCurSel(nSelectionIndex);
		}
	}
}

void ObjectiveDlg::OnOK() 
{
	SaveDialogValues();
	CDialog::OnOK();
}

void ObjectiveDlg::OnSelchangeGroup() 
{
	m_Mech.ResetContent();

	int group = m_modelGroup.GetCurSel();
	group = m_modelGroup.GetItemData( group );

	const char* MechNames[256];
	int count = 256;
		
	EditorObjectMgr::instance()->getBuildingNamesInGroup( group, MechNames, count );

	for ( int i = 0; i < count; ++i )
	{
		m_Mech.AddString( MechNames[i] );
	}


	m_Mech.SetCurSel( 0 );	
	
}

void ObjectiveDlg::OnBaseedit2() 
{
	CWnd* pWnd = GetDlgItem( IDC_BASE2 );
	DoColorBox( pWnd );

	
}

void ObjectiveDlg::OnHighilight2edit2() 
{
	CWnd* pWnd = GetDlgItem( IDC_HIGHLIGHT2 );
	DoColorBox( pWnd );

	
}

void ObjectiveDlg::OnHighlight1edit() 
{
	CWnd* pWnd = GetDlgItem( IDC_HIGHLIGHT1 );
	DoColorBox( pWnd );
	
}

void ObjectiveDlg::DoEditColorChange( long ID )
{
	CString text;
	GetDlgItem( ID )->GetWindowText( text );

	bool bChanged = false;
	int i = 0;
	
	if ( text.GetLength() > 1 && (text[0] == '0' && (text[1] == 'x' || text[i] == 'X' )) )
		i = 2;

	for ( ; i < text.GetLength(); ++i )
	{
		if ( !isxdigit( text[i] ) )
		{
			text.Remove( text[i] );
			bChanged = true;
		}
	}

	if ( bChanged )
		GetDlgItem( ID )->SetWindowText( text );

	GetDlgItem( ID )->RedrawWindow( );
	
}

void ObjectiveDlg::OnChangeHighlight1() 
{
	DoEditColorChange( IDC_HIGHLIGHT1 );
	
}

void ObjectiveDlg::OnChangeHighlight2() 
{
	DoEditColorChange( IDC_HIGHLIGHT2 );
	
}

void ObjectiveDlg::OnChangeBase2() 
{
	DoEditColorChange( IDC_BASE2 );
	
}

void ObjectiveDlg::DoColorBox( CWnd* pWnd )
{
	if ( pWnd )
	{
		CString tmpStr;
		pWnd->GetWindowText( tmpStr );

		tmpStr.Replace( "0x", "" );

		long base;
		sscanf( tmpStr, "%x", &base );
		base &= 0x00ffffff;


		CColorDialog dlg( reverseRGB(base), NULL, this );
		if (IDOK == dlg.DoModal() )
		{
			base = reverseRGB( dlg.GetColor() );

			tmpStr.Format( "0x%x", base );
			pWnd->SetWindowText( tmpStr );
		}
	}
}



HBRUSH ObjectiveDlg::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor) 
{
	HBRUSH hbr = CDialog::OnCtlColor(pDC, pWnd, nCtlColor);
	
	if ( GetDlgItem( IDC_BASE2 )->m_hWnd == pWnd->m_hWnd )
	{
		CString tmpStr;
		pWnd->GetWindowText( tmpStr );

		tmpStr.Replace( "0x", "" );

		long base;
		sscanf( tmpStr, "%x", &base );
		base &= 0x00ffffff;
		base = reverseRGB( base );
		
		if ( baseBrush.m_hObject )
			baseBrush.DeleteObject();

		baseBrush.CreateSolidBrush( base );
		pDC->SetBkColor( base );

		
		if ( ((base & 0xff) + ( (base & 0xff00)>>8 ) + ( (base & 0xff0000)>>16 ))/3 < 85 )
			pDC->SetTextColor( 0x00ffffff );

		return (HBRUSH)baseBrush.m_hObject;
	}

	if ( GetDlgItem( IDC_HIGHLIGHT1 )->m_hWnd == pWnd->m_hWnd )
	{
		CString tmpStr;
		pWnd->GetWindowText( tmpStr );

		tmpStr.Replace( "0x", "" );

		long base;
		sscanf( tmpStr, "%x", &base );
		base &= 0x00ffffff;
		base = reverseRGB( base );

		if ( brush1.m_hObject )
			brush1.DeleteObject();

		brush1.CreateSolidBrush( base );
		pDC->SetBkColor( base );

		if ( ((base & 0xff) + ( (base & 0xff00)>>8 ) + ( (base & 0xff0000)>>16 ))/3 < 85 )
			pDC->SetTextColor( 0x00ffffff );
		return (HBRUSH)brush1.m_hObject;
	}

	if ( GetDlgItem( IDC_HIGHLIGHT2 )->m_hWnd == pWnd->m_hWnd )
	{
		CString tmpStr;
		pWnd->GetWindowText( tmpStr );
		
		tmpStr.Replace( "0x", "" );

		long base;
		sscanf( tmpStr, "%x", &base );
		base &= 0x00ffffff;
		base = reverseRGB( base );

		
		if ( brush2.m_hObject )
			brush2.DeleteObject();

		brush2.CreateSolidBrush( base );
		pDC->SetBkColor( base );
		
		if ( ((base & 0xff) + ( (base & 0xff00)>>8 ) + ( (base & 0xff0000)>>16 ))/3 < 85 )
			pDC->SetTextColor( 0x00ffffff );

		return (HBRUSH)brush2.m_hObject;
	}

		
	
	return hbr;
}

void ObjectiveDlg::OnObjectiveTitleEditButton() 
{
	CUserTextEdit userTextEditDialog;
	userTextEditDialog.m_UnlocalizedText = m_TitleUnlocalizedText;
	userTextEditDialog.m_UseResourceString = m_TitleUseResourceString;
	userTextEditDialog.m_ResourceStringID = m_TitleResourceStringID;
	int ret = userTextEditDialog.DoModal();
	if (IDOK == ret) {
		m_TitleUnlocalizedText = userTextEditDialog.m_UnlocalizedText;
		m_TitleUseResourceString = userTextEditDialog.m_UseResourceString;
		m_TitleResourceStringID = userTextEditDialog.m_ResourceStringID;
		UpdateTitleDisplay();
	}
}

void ObjectiveDlg::OnObjectiveDescriptionEditButton() 
{
	CUserTextEdit userTextEditDialog;
	userTextEditDialog.m_UnlocalizedText = m_DescriptionUnlocalizedText;
	userTextEditDialog.m_UseResourceString = m_DescriptionUseResourceString;
	userTextEditDialog.m_ResourceStringID = m_DescriptionResourceStringID;
	int ret = userTextEditDialog.DoModal();
	if (IDOK == ret) {
		m_DescriptionUnlocalizedText = userTextEditDialog.m_UnlocalizedText;
		m_DescriptionUseResourceString = userTextEditDialog.m_UseResourceString;
		m_DescriptionResourceStringID = userTextEditDialog.m_ResourceStringID;
		UpdateDescriptionDisplay();
	}
}

void ObjectiveDlg::OnObjectiveAddFailureConditionButton() 
{
	{
		DWORD comboboxSelection = (&m_FailureConditionComboBox)->GetCurSel();
		if ((0 <= comboboxSelection) && (failureConditionComboBoxItems.Count() > comboboxSelection)) {
			//condition_species_type species = failureConditionComboBoxItems[comboboxSelection];
			condition_species_type species = *(failureConditionComboBoxItems.Iterator(comboboxSelection));
			pNewCondition = CObjective::new_CObjectiveCondition(species, m_ModifiedObjective.Alignment());
		} else {
			return;
		}
	}
	assert(pNewCondition);

	/* This call may set the editor into ObjectSelectOnlyMode (i.e.: set the  value of
	EditorInterface::instance()->ObjectSelectOnlyMode() to true) */
	bool result = pNewCondition->EditDialog();

	if (EditorInterface::instance()->ObjectSelectOnlyMode()) {
		/* close the dialog and enter ObjectSelectOnlyMode */
		SaveDialogValues();
		EditorInterface::instance()->objectivesEditState.ModifiedObjective = m_ModifiedObjective;
		EditorInterface::instance()->objectivesEditState.listID = CObjectivesEditState::FAILURE_CONDITION;
		EditorInterface::instance()->objectivesEditState.nConditionSpeciesSelectionIndex = (&m_ComboBox)->GetCurSel();
		EditorInterface::instance()->objectivesEditState.nActionSpeciesSelectionIndex = (&m_ActionComboBox)->GetCurSel();
		EditorInterface::instance()->objectivesEditState.nFailureConditionSpeciesSelectionIndex = (&m_FailureConditionComboBox)->GetCurSel();
		EditorInterface::instance()->objectivesEditState.nFailureActionSpeciesSelectionIndex = (&m_FailureActionComboBox)->GetCurSel();
		delete pNewCondition; pNewCondition = 0;
		EndDialog(IDOK);
		return;
	} else {
		if (true == result) {
			m_ModifiedObjective.m_failureConditionList.Append(pNewCondition);
			m_FailureConditionList.SetCurSel(m_FailureConditionList.GetCount() - 1);
			syncConditionsListWithListBox(&(m_ModifiedObjective.m_failureConditionList), (&m_FailureConditionList));
		} else {
			delete pNewCondition; pNewCondition = 0;
		}
		return;
	}
}

void ObjectiveDlg::OnObjectiveRemoveFailureConditionButton() 
{
	int nSelectionIndex = (&m_FailureConditionList)->GetCurSel();
	if ((0 <= nSelectionIndex) && (m_ModifiedObjective.m_failureConditionList.Count() > nSelectionIndex)) {
		// should put up confirmation box here
		delete *(m_ModifiedObjective.m_failureConditionList.Iterator(nSelectionIndex));
		m_ModifiedObjective.m_failureConditionList.Delete(nSelectionIndex);
		syncConditionsListWithListBox(&(m_ModifiedObjective.m_failureConditionList), (&m_FailureConditionList));
		if (0 < m_FailureConditionList.GetCount()) {
			if (m_FailureConditionList.GetCount() <= (long)nSelectionIndex) {
				nSelectionIndex = m_FailureConditionList.GetCount() - 1;
			}
			m_FailureConditionList.SetCurSel(nSelectionIndex);
		}
	}
}

void ObjectiveDlg::OnObjectiveAddFailureActionButton() 
{
	{
		DWORD actionComboboxSelection = (&m_FailureActionComboBox)->GetCurSel();
		if ((0 <= actionComboboxSelection) && (failureActionComboBoxItems.Count() > actionComboboxSelection)) {
			//action_species_type species = failureActionComboBoxItems[actionComboboxSelection];
			action_species_type species = *(failureActionComboBoxItems.Iterator(actionComboboxSelection));
			pNewAction = CObjective::new_CObjectiveAction(species, m_ModifiedObjective.Alignment());
		} else {
			return;
		}
	}
	assert(pNewAction);

	/* This call may set the editor into ObjectSelectOnlyMode (i.e.: set the  value of
	EditorInterface::instance()->ObjectSelectOnlyMode() to true) */
	bool result = pNewAction->EditDialog();

	if (EditorInterface::instance()->ObjectSelectOnlyMode()) {
		/* close the dialog and enter ObjectSelectOnlyMode */
		SaveDialogValues();
		EditorInterface::instance()->objectivesEditState.ModifiedObjective = m_ModifiedObjective;
		EditorInterface::instance()->objectivesEditState.nConditionSpeciesSelectionIndex = (&m_ComboBox)->GetCurSel();
		EditorInterface::instance()->objectivesEditState.nActionSpeciesSelectionIndex = (&m_FailureActionComboBox)->GetCurSel();
		EditorInterface::instance()->objectivesEditState.nFailureConditionSpeciesSelectionIndex = (&m_FailureConditionComboBox)->GetCurSel();
		EditorInterface::instance()->objectivesEditState.nFailureActionSpeciesSelectionIndex = (&m_FailureActionComboBox)->GetCurSel();
		delete pNewAction; pNewAction = 0;
		EndDialog(IDOK);
		return;
	} else {
		if (true == result) {
			m_ModifiedObjective.m_failureActionList.Append(pNewAction);
			m_FailureActionList.SetCurSel(m_FailureActionList.GetCount() - 1);
			syncActionsListWithListBox(&(m_ModifiedObjective.m_failureActionList), (&m_FailureActionList));
		} else {
			delete pNewAction; pNewAction = 0;
		}
		return;
	}
}

void ObjectiveDlg::OnObjectiveRemoveFailureActionButton() 
{
	int nSelectionIndex = (&m_FailureActionList)->GetCurSel();
	if ((0 <= nSelectionIndex) && (m_ModifiedObjective.m_failureActionList.Count() > nSelectionIndex)) {
		// should put up confirmation box here
		delete *(m_ModifiedObjective.m_failureActionList.Iterator(nSelectionIndex));
		m_ModifiedObjective.m_failureActionList.Delete(nSelectionIndex);
		syncActionsListWithListBox(&(m_ModifiedObjective.m_failureActionList), (&m_FailureActionList));
		if (0 < m_FailureActionList.GetCount()) {
			if (m_FailureActionList.GetCount() <= (long)nSelectionIndex) {
				nSelectionIndex = m_FailureActionList.GetCount() - 1;
			}
			m_FailureActionList.SetCurSel(nSelectionIndex);
		}
	}
}

void ObjectiveDlg::OnObjectiveViewConditionButton() 
{
	int nSelectionIndex = m_List.GetCurSel();
	if ((LB_ERR != nSelectionIndex) && (0 <= nSelectionIndex) && (m_ModifiedObjective.Count() > nSelectionIndex)) {
		CObjective::condition_list_type::EConstIterator it = m_ModifiedObjective.Begin();
		for (int i = 0; i < nSelectionIndex && !it.IsDone(); ++i) it++;
		if (!it.IsDone()) {
			EString tmpEStr = objReadableCondition(*it);
			ViewConditionOrActionDlg viewDlg;
			viewDlg.m_Edit = tmpEStr.Data();
			viewDlg.DoModal();
		}
	}
}

void ObjectiveDlg::OnObjectiveViewActionButton() 
{
	int nSelectionIndex = m_ActionList.GetCurSel();
	if ((LB_ERR != nSelectionIndex) && (0 <= nSelectionIndex) && (m_ModifiedObjective.m_actionList.Count() > nSelectionIndex)) {
		CObjective::action_list_type::EConstIterator it = m_ModifiedObjective.m_actionList.Begin();
		for (int i = 0; i < nSelectionIndex && !it.IsDone(); ++i) it++;
		if (!it.IsDone()) {
			EString tmpEStr = objReadableAction(*it);
			ViewConditionOrActionDlg viewDlg;
			viewDlg.m_Edit = tmpEStr.Data();
			viewDlg.DoModal();
		}
	}
}

void ObjectiveDlg::OnObjectiveViewFailureConditionButton() 
{
	int nSelectionIndex = m_FailureConditionList.GetCurSel();
	if ((LB_ERR != nSelectionIndex) && (0 <= nSelectionIndex) && (m_ModifiedObjective.m_failureConditionList.Count() > nSelectionIndex)) {
		CObjective::condition_list_type::EConstIterator it = m_ModifiedObjective.m_failureConditionList.Begin();
		for (int i = 0; i < nSelectionIndex && !it.IsDone(); ++i) it++;
		if (!it.IsDone()) {
			EString tmpEStr = objReadableCondition(*it);
			ViewConditionOrActionDlg viewDlg;
			viewDlg.m_Edit = tmpEStr.Data();
			viewDlg.DoModal();
		}
	}
}

void ObjectiveDlg::OnObjectiveViewFailureActionButton() 
{
	int nSelectionIndex = m_FailureActionList.GetCurSel();
	if ((LB_ERR != nSelectionIndex) && (0 <= nSelectionIndex) && (m_ModifiedObjective.m_failureActionList.Count() > nSelectionIndex)) {
		CObjective::action_list_type::EConstIterator it = m_ModifiedObjective.m_failureActionList.Begin();
		for (int i = 0; i < nSelectionIndex && !it.IsDone(); ++i) it++;
		if (!it.IsDone()) {
			EString tmpEStr = objReadableAction(*it);
			ViewConditionOrActionDlg viewDlg;
			viewDlg.m_Edit = tmpEStr.Data();
			viewDlg.DoModal();
		}
	}
}

void ObjectiveDlg::OnObjectiveClearLogisticsModelButton() 
{
	m_Mech.ResetContent();
	m_modelGroup.SetCurSel(-1);
}
