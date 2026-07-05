#define LOGISTICSSCREEN_CPP
/*************************************************************************************************\
LogisticsScreen.cpp			: Implementation of the LogisticsScreen component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================// 
\*************************************************************************************************/

#include"logisticsscreen.h"
#include"inifile.h"
#include"asystem.h"
#include"abutton.h"
#include"aedit.h"
#include"err.h"
#include"aanimobject.h"
#include"UiDefs.h"
#include "../GuiRuntime/GuiRuntime.h"
#include "../GameOS/gameos/gos_profiler.h"

extern long helpTextID;
extern long helpTextHeaderID;
extern float frameLength;


LogisticsScreen::LogisticsScreen()
{
	statics = 0;
	rects = 0;
	buttons = 0;
	edits = 0;
	textObjects = 0;
	animObjects = 0;	
	defsUiPage = 0;
	staticCount = rectCount = buttonCount = textCount = editCount = animObjectsCount = 0;

	helpTextArrayID = -1;
	
	
	fadeInTime = fadeOutTime= fadeTime = 0;
	fadeOutMaxColor = 0xff000000;

	
}

//-------------------------------------------------------------------------------------------------

LogisticsScreen::~LogisticsScreen()
{
	destroy();
}

void LogisticsScreen::destroy()
{
	clear();	

}

void	LogisticsScreen::clear()
{
	if ( statics )
		delete [] statics;

	if ( rects )
		delete [] rects;

	if ( buttons )
		delete  [] buttons;

	if ( edits )
		delete [] edits;

	if ( textObjects )
		delete[] textObjects;

	if ( animObjects )
		delete [] animObjects;

	statics = 0;
	rects = 0;
	buttons = 0;
	edits = 0;
	textObjects = 0;
	animObjects = 0;

	delete defsUiPage;
	defsUiPage = 0;

	staticCount = 0;
	rectCount = 0;
	buttonCount = 0;
	editCount= 0;
	textCount = 0;
	animObjectsCount= 0;
}



//-------------------------------------------------------------------------------------------------

void LogisticsScreen::init( FitIniFile& file, const char* staticName, const char* textName, const char* rectName,
					  const char* buttonName, const char* editName, const char* animObjectName, DWORD neverFlush )
{
	ZoneScopedN("LogisticsScreen::init");
	clear();
	
	char blockName[256];

	// init statics
	if ( staticName )
	{
		ZoneScopedN("LogisticsScreen::init statics");
		sprintf( blockName, "%s%c", staticName, 's' );
		if ( NO_ERR == file.seekBlock( blockName ) )
		{
			file.readIdLong( "staticCount", staticCount );

			if ( staticCount )
			{
				{
					ZoneScopedN("LogisticsScreen::init statics alloc");
					statics = new aObject[staticCount];
				}

				char blockName[128];
				for ( int i = 0; i < staticCount; i++ )
				{
					ZoneScopedN("LogisticsScreen::init static");
					sprintf( blockName, "%s%d", staticName, i );
					statics[i].init( &file, blockName );			
				}
				
			}
		}
	}

	if ( rectName )
	{
		ZoneScopedN("LogisticsScreen::init rects");
		// init rects
		sprintf( blockName, "%s%c", rectName, 's' );
		if ( NO_ERR == file.seekBlock( blockName ) )
		{
			file.readIdLong( "rectCount", rectCount );
			if ( rectCount )
			{
				{
					ZoneScopedN("LogisticsScreen::init rects alloc");
					rects = new aRect[rectCount];
				}

				char blockName[128];
				for ( int i = 0; i < rectCount; i++ )
				{
					ZoneScopedN("LogisticsScreen::init rect");
					sprintf( blockName, "%s%d", rectName, i );
					rects[i].init( &file, blockName );
				}
			}
		}
	}

	
	// init buttons
	if ( buttonName )
	{
		ZoneScopedN("LogisticsScreen::init buttons");
		sprintf( blockName, "%s%c", buttonName, 's' );
		if ( NO_ERR == file.seekBlock( blockName ) )
		{
			file.readIdLong( "buttonCount", buttonCount );

			if ( buttonCount )
			{
				char blockName[128];
				{
					ZoneScopedN("LogisticsScreen::init buttons alloc");
					buttons = new aAnimButton[buttonCount];
				}
				for ( int i = 0; i < buttonCount; i++ )
				{
					ZoneScopedN("LogisticsScreen::init button");
					sprintf( blockName,"%s%d", buttonName, i );
					buttons[i].init( file, blockName );
					addChild( &buttons[i] );
				}
			}
		
		}
	}

	// init texts
	if ( textName )
	{
		ZoneScopedN("LogisticsScreen::init texts");
		sprintf( blockName, "%s%c", textName, 's' );
		if ( NO_ERR == file.seekBlock( blockName ) )
		{
			if ( NO_ERR != file.readIdLong( "TextEntryCount", textCount ) )
				file.readIdLong( "TextCount", textCount );

			if ( textCount )
			{
				{
					ZoneScopedN("LogisticsScreen::init texts alloc");
					textObjects = new aText[textCount];
				}
				char blockName[64];
				for ( int i = 0; i < textCount; i++ )
				{
					ZoneScopedN("LogisticsScreen::init text");
					sprintf( blockName, "%s%d", textName, i );
					textObjects[i].init( &file, blockName );
				}
				
			}
		}
	}

	if ( editName )
	{
		ZoneScopedN("LogisticsScreen::init edits");
		sprintf( blockName, "%s%c", editName, 's' );
		if ( NO_ERR == file.seekBlock( blockName ) )
		{
			if ( NO_ERR != file.readIdLong( "EditCount", editCount ) )
				file.readIdLong( "EditCount", editCount );

			if ( editCount )
			{
				{
					ZoneScopedN("LogisticsScreen::init edits alloc");
					edits = new aEdit[editCount];
				}
				char blockName[64];
				for ( int i = 0; i < editCount; i++ )
				{
					ZoneScopedN("LogisticsScreen::init edit");
					sprintf( blockName, "%s%d", editName, i );
					edits[i].init( &file, blockName );
				}
				
			}
		}
	}

	if ( animObjectName )
	{
		ZoneScopedN("LogisticsScreen::init animObjects");
		sprintf( blockName, "%s%c", animObjectName, 's' );
		if ( NO_ERR == file.seekBlock( blockName ) )
		{
			file.readIdLong( "Count", animObjectsCount );

			if ( animObjectsCount )
			{
				{
					ZoneScopedN("LogisticsScreen::init animObjects alloc");
					animObjects = new aAnimObject[animObjectsCount];
				}
				char blockName[64];
				for ( int i = 0; i < animObjectsCount; i++ )
				{
					ZoneScopedN("LogisticsScreen::init animObject");
					sprintf( blockName, "%s%d", animObjectName, i );
					animObjects[i].init( &file, blockName, neverFlush );
				}
				
			}
		}
	}

	if (UiDefs::gameOsUiDefsEnabled())
	{
		const std::string replacementPath = UiDefs::replacementPathForLegacyFit(file.getFilename());
		if (!replacementPath.empty())
		{
			defsUiPage = new UiDefs::GameOSPage();
			if (!defsUiPage->load(replacementPath.c_str()))
			{
				delete defsUiPage;
				defsUiPage = 0;
			}
		}
	}
}

bool LogisticsScreen::tryInitDefsOnly(const char* legacyFitPath)
{
	clear();
	if (!UiDefs::gameOsUiDefsEnabled() || !legacyFitPath || !*legacyFitPath)
		return false;
	const std::string replacementPath = UiDefs::replacementPathForLegacyFit(legacyFitPath);
	if (replacementPath.empty())
		return false;
	defsUiPage = new UiDefs::GameOSPage();
	if (!defsUiPage->load(replacementPath.c_str()))
	{
		delete defsUiPage;
		defsUiPage = nullptr;
		return false;
	}
	return true;
}

aButton* LogisticsScreen::getButton( long who )
{
	for ( int i = 0; i < buttonCount; i++ )
	{
		if ( buttons[i].getID() == who )
		{
			return &buttons[i];
		}
	}

	return NULL;
}

//-------------------------------------------------------------------------------------------------
aButton* LogisticsScreen::getButtonByIndex( long index )
{
	if ( index >= 0 && index < buttonCount )
		return &buttons[index];

	return NULL;
}

aRect* LogisticsScreen::getRect( long who )
{
	if ((who >= 0) && (who < rectCount))
	{
		return &rects[who];
	}

	return NULL;
}

//-------------------------------------------------------------------------------------------------
void LogisticsScreen::update()
{
	for ( int i = 0; i < staticCount; i++ )
	{
		statics[i].update();
	}

	if (defsUiPage && defsUiPage->isLoaded())
	{
		// Sub-pages mounted inside a parent screen (e.g. options tabs inside
		// OptionsScreenWrapper) live at a non-zero global offset; the hit-test
		// transform in GameOSPage::update must match the same offset used by
		// render() below or clicks land on the wrong elements.
		defsUiPage->update(this, globalX(), globalY());
	}
	else
	{
		for (int i = 0; i < buttonCount; i++ )
		{
			buttons[i].update();
		}
	}

	for (int i = 0; i < textCount; i++ )
		textObjects[i].update();

	for (int i = 0; i < rectCount; i++ )
		rects[i].update();

	// help text
	if ( helpTextArrayID != -1 )
	{
		if ( ::helpTextID )
		{

			EString helpText;
			char tmp[1024];
		//	if ( helpTextHeaderID )
		//	{
		//		cLoadString( helpTextHeaderID, tmp, 255 );
		//		helpText = tmp;
		//		helpText.MakeUpper();
		//		helpText += '\n';
		//	}

			cLoadString( helpTextID, tmp, 1024 );
			helpText = tmp;
			textObjects[helpTextArrayID].setText( helpText );
			if ( !defsHelpTextKey.empty() )
				setDefsElementText( defsHelpTextKey, tmp );
		}
		else
		{
			textObjects[helpTextArrayID].setText( "" );
			if ( !defsHelpTextKey.empty() )
				setDefsElementText( defsHelpTextKey, "" );
		}
	}

	if (!hasDefsEditBox()) {
		for (int i = 0; i < editCount; i++ )
			edits[i].update();
	}

	for (int i = 0; i < animObjectsCount; i++ )
		animObjects[i].update();

//	if ( gos_GetKeyStatus( KEY_RETURN ) == KEY_RELEASED )
//	{
//		if ( getButton( 50 /*MB_MSG_NEXT*/ ) )
//		{
//			if ( getButton(50 )->isEnabled() )
//				handleMessage( aMSG_LEFTMOUSEDOWN, 50 );
//		}
//	}
//	if ( gos_GetKeyStatus( KEY_ESCAPE ) == KEY_RELEASED )
//	{
//		if ( getButton( 57 /*MB_MSG_MAINMENU*/ ) )
//		{
//			if ( getButton(57 )->isEnabled() )
//				handleMessage( aMSG_LEFTMOUSEDOWN, 57 );
//		}

//	}

	helpTextID = 0;


}


bool LogisticsScreen::hasDefsUiPage() const
{
	return defsUiPage && defsUiPage->isLoaded();
}

bool LogisticsScreen::allAnimObjectsDone() const
{
	for ( int i = 0; i < animObjectsCount; i++ )
	{
		if ( !animObjects[i].isDone() )
			return false;
	}
	return true;
}

//-------------------------------------------------------------------------------------------------
void LogisticsScreen::render()
{
	if ( !isShowing() )
		return;

	if (defsUiPage && defsUiPage->isLoaded())
	{
		// Passthrough: render the full legacy element set first so legacy
		// content (options controls, planet art, etc.) shows beneath the overlay.
		if (defsUiPage->isLegacyPassthrough()) {
			for (int i = 0; i < rectCount; i++)
				if (!rects[i].bOutline && (rects[i].getColor() & 0xff000000) == 0xff000000)
					rects[i].render();
			for (int i = 0; i < staticCount; i++)
				statics[i].render();
			for (int i = 0; i < rectCount; i++)
				if (rects[i].bOutline)
					rects[i].render();
			for (int i = 0; i < rectCount; i++)
				if ((rects[i].getColor() & 0xff000000) != 0xff000000)
					rects[i].render();
			for (int i = 0; i < buttonCount; i++)
				buttons[i].render();
			for (int i = 0; i < textCount; i++)
				textObjects[i].render();
			for (int i = 0; i < editCount; i++)
				edits[i].render();
			for (int i = 0; i < animObjectsCount; i++)
				animObjects[i].render();
		}

		// Legacy aEdit renders only when the defs page has no visible GuiEditBox
		// for this screen.  When GuiEditBox is present it owns the edit widget
		// and renders in the ImGui window layer; the legacy edit would double-draw.
		if (!defsUiPage->isLegacyPassthrough() && !hasDefsEditBox()) {
			for ( int i = 0; i < editCount; i++ )
				edits[i].render();
		}

		// Static GuiAnimation snapshots stand down when the screen owns live
		// legacy animObjects; the aObject GUI bridge below renders the real
		// objects with their actual keyframe playback (fades, slides).
		defsUiPage->setSuppressAnimationElements( animObjectsCount > 0 && !defsUiPage->isLegacyPassthrough() );
		// Sub-pages mounted inside a parent screen (e.g. options tabs inside
		// OptionsScreenWrapper, mounted at a non-zero offset within the
		// metal frame) draw at raw page-local coordinates unless the
		// parent's global position is folded into the scale transform here.
		defsUiPage->render(globalX(), globalY());

		if ( animObjectsCount > 0 )
		{
			// animObject coordinates live in legacy Environment space; the
			// ImGui HUD layer draws in display space.
			float dw = 0.f;
			float dh = 0.f;
			float sx = 1.f;
			float sy = 1.f;
			if ( GuiRuntime::GetDisplaySize( dw, dh ) &&
				 Environment.screenWidth > 0 && Environment.screenHeight > 0 )
			{
				sx = dw / (float)Environment.screenWidth;
				sy = dh / (float)Environment.screenHeight;
			}
			aObject::beginGuiBridge( sx, sy );
			for ( int i = 0; i < animObjectsCount; i++ )
				animObjects[i].render();
			aObject::endGuiBridge();
		}

		// Fades must go through the ImGui layer here: the legacy drawRect is
		// a GameOS HUD draw, which composites BEFORE GuiRuntime::Render(), so
		// it would dim the 3D scene but sit underneath the defs UI page.
		if ( fadeOutTime )
		{
			fadeTime += frameLength;
			long color = interpolateColor( 0,fadeOutMaxColor, fadeTime/fadeOutTime );
			GuiRuntime::DrawUiRect( 0.f, 0.f,
				(float)Environment.screenWidth, (float)Environment.screenHeight,
				(unsigned int)color, true );
		}
		else if ( fadeInTime && fadeInTime > fadeTime )
		{
			fadeTime += frameLength;
			long color = interpolateColor( fadeOutMaxColor, 0, fadeTime/fadeInTime );
			GuiRuntime::DrawUiRect( 0.f, 0.f,
				(float)Environment.screenWidth, (float)Environment.screenHeight,
				(unsigned int)color, true );
		}
		return;
	}

	for (int i = 0; i < rectCount; i++ )
	{
		if ( !rects[i].bOutline && 
			( (rects[i].getColor() & 0xff000000) == 0xff000000 ) )
			rects[i].render();
	}


	for (int i = 0; i < staticCount; i++ )
		statics[i].render();

	for (int i = 0; i < rectCount; i++ )
	{
		if ( rects[i].bOutline )
			rects[i].render();
	}

	// transparencies after statics
	for (int i = 0; i < rectCount; i++ )
	{
		if ( (rects[i].getColor() & 0xff000000) != 0xff000000 )
			rects[i].render();
	}


	for (int  i = 0; i < buttonCount; i++ )
		buttons[i].render();

	for (int  i = 0; i < textCount; i++ )
	{
		textObjects[i].render();
	}

	for (int  i = 0; i < editCount; i++ )
		edits[i].render( );

	for (int  i = 0; i < animObjectsCount; i++ )
		animObjects[i].render();



	if ( fadeOutTime )
	{
		fadeTime += frameLength;
		long color = interpolateColor( 0,fadeOutMaxColor, fadeTime/fadeOutTime );
		GUI_RECT rect = { 0,0, Environment.screenWidth, Environment.screenHeight };
		drawRect( rect, color );
	}
	else if ( fadeInTime && fadeInTime > fadeTime )
	{
		fadeTime += frameLength;
		long color = interpolateColor( fadeOutMaxColor, 0, fadeTime/fadeInTime );
		GUI_RECT rect = { 0,0, Environment.screenWidth, Environment.screenHeight };
		drawRect( rect, color );
	}
	

}

void LogisticsScreen::renderLegacy()
{
	if ( !isShowing() )
		return;

	for (int i = 0; i < rectCount; i++)
		if (!rects[i].bOutline && (rects[i].getColor() & 0xff000000) == 0xff000000)
			rects[i].render();

	for (int i = 0; i < staticCount; i++)
		statics[i].render();

	for (int i = 0; i < rectCount; i++)
		if (rects[i].bOutline)
			rects[i].render();

	for (int i = 0; i < rectCount; i++)
		if ((rects[i].getColor() & 0xff000000) != 0xff000000)
			rects[i].render();

	for (int i = 0; i < buttonCount; i++)
		buttons[i].render();

	for (int i = 0; i < textCount; i++)
		textObjects[i].render();

	for (int i = 0; i < editCount; i++)
		edits[i].render();

	for (int i = 0; i < animObjectsCount; i++)
		animObjects[i].render();
}

long LogisticsScreen::getStatus()
{
	if ( status != RUNNING && fadeOutTime )
	{
		if ( fadeTime > fadeOutTime )
		{
			return status;
		}
		else
			return RUNNING; // fake it until done fading
	}

	return status;
}

void LogisticsScreen::render( int xOffset, int yOffset )
{
	if ( !isShowing() )
		return;

	if (defsUiPage && defsUiPage->isLoaded())
	{
		// Passthrough: render legacy first so it appears beneath the overlay.
		if (defsUiPage->isLegacyPassthrough()) {
			for (int i = 0; i < rectCount; i++)
				if (!rects[i].bOutline && (rects[i].getColor() & 0xff000000) == 0xff000000)
					rects[i].render();
			for (int i = 0; i < staticCount; i++)
				statics[i].render();
			for (int i = 0; i < rectCount; i++)
				if (rects[i].bOutline)
					rects[i].render();
			for (int i = 0; i < rectCount; i++)
				if ((rects[i].getColor() & 0xff000000) != 0xff000000)
					rects[i].render();
			for (int i = 0; i < buttonCount; i++)
				buttons[i].render();
			for (int i = 0; i < textCount; i++)
				textObjects[i].render();
			for (int i = 0; i < editCount; i++) {
				edits[i].move(xOffset, yOffset);
				edits[i].render();
				edits[i].move(-xOffset, -yOffset);
			}
			for (int i = 0; i < animObjectsCount; i++) {
				animObjects[i].move(xOffset, yOffset);
				animObjects[i].render();
				animObjects[i].move(-xOffset, -yOffset);
			}
		}

		// Same as render(): legacy aEdit only when no visible GuiEditBox owns it.
		if (!defsUiPage->isLegacyPassthrough() && !hasDefsEditBox()) {
			for ( int i = 0; i < editCount; i++ )
			{
				edits[i].move( xOffset, yOffset );
				edits[i].render();
				edits[i].move( -xOffset, -yOffset );
			}
		}

		defsUiPage->setSuppressAnimationElements( animObjectsCount > 0 && !defsUiPage->isLegacyPassthrough() );
		defsUiPage->render(globalX() + xOffset, globalY() + yOffset);

		if ( animObjectsCount > 0 )
		{
			float dw = 0.f;
			float dh = 0.f;
			float sx = 1.f;
			float sy = 1.f;
			if ( GuiRuntime::GetDisplaySize( dw, dh ) &&
				 Environment.screenWidth > 0 && Environment.screenHeight > 0 )
			{
				sx = dw / (float)Environment.screenWidth;
				sy = dh / (float)Environment.screenHeight;
			}
			aObject::beginGuiBridge( sx, sy );
			for ( int i = 0; i < animObjectsCount; i++ )
			{
				// Same move/render/restore the legacy offset path uses; the
				// offsets are Environment-space and scale inside the bridge.
				animObjects[i].move( xOffset, yOffset );
				animObjects[i].render();
				animObjects[i].move( -xOffset, -yOffset );
			}
			aObject::endGuiBridge();
		}

		// Same as render(): fades over a defs UI page must composite in the
		// ImGui layer, not as a GameOS HUD draw underneath it.
		if ( fadeOutTime )
		{
			fadeTime += frameLength;
			long color = interpolateColor( 0,0xff000000, fadeTime/fadeOutTime );
			GuiRuntime::DrawUiRect( 0.f, 0.f,
				(float)Environment.screenWidth, (float)Environment.screenHeight,
				(unsigned int)color, true );
		}
		else if ( fadeInTime && fadeInTime > fadeTime )
		{
			fadeTime += frameLength;
			long color = interpolateColor( 0xff000000, 0, fadeTime/fadeInTime );
			GuiRuntime::DrawUiRect( 0.f, 0.f,
				(float)Environment.screenWidth, (float)Environment.screenHeight,
				(unsigned int)color, true );
		}
		return;
	}
	
	for (int i = 0; i < rectCount; i++ )
	{
		if ( !rects[i].bOutline&& 
			( (rects[i].getColor() & 0xff000000) == 0xff000000 ) )
		{
			rects[i].move( xOffset, yOffset );
			rects[i].render();
			rects[i].move( -xOffset, -yOffset );
		}
	}


	for (int  i = 0; i < staticCount; i++ )
	{
		statics[i].move( xOffset, yOffset );
		statics[i].render();
		statics[i].move( -xOffset, -yOffset );
	}

	for (int  i = 0; i < rectCount; i++ )
	{
		if ( rects[i].bOutline )
		{
			rects[i].move( xOffset, yOffset );
			rects[i].render();
			rects[i].move( -xOffset, -yOffset );
		}
	}

	// transparencies after statics
	for (int  i = 0; i < rectCount; i++ )
	{
		if ( (rects[i].getColor() & 0xff000000) != 0xff000000 )
		{
			rects[i].move( xOffset, yOffset );
			rects[i].render();
			rects[i].move( -xOffset, -yOffset );
		}
	}

	for (int  i = 0; i < buttonCount; i++ )
	{
		buttons[i].move( xOffset, yOffset );
		buttons[i].render();
		buttons[i].move( -xOffset, -yOffset );
	}

	for (int  i = 0; i < textCount; i++ )
	{
		textObjects[i].move( xOffset, yOffset );
		textObjects[i].render();
		textObjects[i].move( -xOffset, -yOffset );
	}

	for (int  i = 0; i < editCount; i++ )
	{
		edits[i].move( xOffset, yOffset );
		edits[i].render();
		edits[i].move( -xOffset, -yOffset );
	}

	for (int  i = 0; i < animObjectsCount; i++ )
	{
		animObjects[i].move( xOffset, yOffset );
		animObjects[i].render();
		animObjects[i].move( -xOffset, -yOffset );
	}

	if ( fadeOutTime )
	{
		fadeTime += frameLength;
		long color = interpolateColor( 0,0xff000000, fadeTime/fadeOutTime );
		GUI_RECT rect = { 0,0, Environment.screenWidth, Environment.screenHeight };
		drawRect( rect, color );
	}
	else if ( fadeInTime && fadeInTime > fadeTime )
	{
		fadeTime += frameLength;
		long color = interpolateColor( 0xff000000, 0, fadeTime/fadeInTime );
		GUI_RECT rect = { 0,0, Environment.screenWidth, Environment.screenHeight };
		drawRect( rect, color );
	}




}

LogisticsScreen::LogisticsScreen( const LogisticsScreen& src )
{
	copyData( src );
}
LogisticsScreen& LogisticsScreen::operator=( const LogisticsScreen& src )
{
	copyData( src );
	return *this;
}

void LogisticsScreen::copyData( const LogisticsScreen& src )
{
	if ( &src != this )
	{
		destroy();
		defsUiPage = 0;

		rectCount = src.rectCount;
		if ( rectCount )
		{
			rects = new aRect[rectCount];
			for (int i = 0; i < src.rectCount; i++ )
			{
				rects[i] = (src.rects[i]);
			}
		}

		staticCount = src.staticCount;
		if ( staticCount )
		{
			statics = new aObject[staticCount];
			for ( int i = 0; i < staticCount; i++ )
			{  
				statics[i] = src.statics[i];
			}
		}

		buttonCount = src.buttonCount;
		if ( buttonCount )
		{
			buttons = new aAnimButton[buttonCount];
			for ( int i = 0; i < buttonCount; i++ )
			{
				buttons[i] = src.buttons[i];
			}
		}

		textCount = src.textCount;
		if ( textCount )
		{
			textObjects = new aText[textCount];
			for ( int i = 0; i < textCount; i++ )
			{
				textObjects[i] = src.textObjects[i];
			}
		}

		animObjectsCount = src.animObjectsCount;
		if ( animObjectsCount )
		{
			animObjects = new aAnimObject[animObjectsCount];
			for ( int i = 0; i < animObjectsCount; i++ )
				animObjects[i] = src.animObjects[i];
		}

		editCount = src.editCount;
		if ( editCount )
		{
			edits = new aEdit[editCount];
			for ( int i = 0; i < editCount; i++ )
				edits[i] = src.edits[i];
		}

	}
}

void  LogisticsScreen::moveTo( long xPos, long yPos )
{
	long xOffset = xPos - globalX();
	long yOffset = yPos - globalY();

	aObject::init( xPos, yPos, 800, 600 );


	move( xOffset, yOffset );
}

void  LogisticsScreen::move( long xOffset, long yOffset )
{
	for (int i = 0; i < rectCount; i++ )
	{
		rects[i].move( xOffset, yOffset );
	}


	for (int  i = 0; i < staticCount; i++ )
	{
		statics[i].move( xOffset, yOffset );
	}

	for (int  i = 0; i < buttonCount; i++ )
	{
		buttons[i].move( xOffset, yOffset );
	}

	for (int  i = 0; i < textCount; i++ )
	{
		textObjects[i].move( xOffset, yOffset );
	}

	for (int  i = 0; i < editCount; i++ )
		edits[i].move( xOffset, yOffset );

	for (int  i = 0; i < animObjectsCount; i++ )
		animObjects[i].move( xOffset, yOffset );

}


bool	LogisticsScreen::inside( long x, long y)
{
	if (defsUiPage && defsUiPage->isLoaded())
		return defsUiPage->inside(x, y, globalX(), globalY());

	for ( int i = 0; i < staticCount; i++ )
	{
		if ( statics[i].pointInside( x, y ) )
			return true;
	}

	for (int  i = 0; i < buttonCount; i++ )
	{
		if ( buttons[i].pointInside( x, y ) )
			return true;
	}

	for (int  i = 0; i < textCount; i++ )
	{
		if ( 	textObjects[i].pointInside( x, y ) )
			return true;
	}

	for (int  i = 0; i < rectCount; i++ )
	{
		if ( rects[i].pointInside( x, y ) )
			return true;
	}

	for (int  i = 0; i < animObjectsCount; i++ )
	{
		if ( animObjects[i].pointInside( x, y ) )
			return true;
	}

		return false;

	return false;
}

void LogisticsScreen::begin()
{

	for ( int i = 0; i < animObjectsCount; i++ )
		animObjects[i].begin();

	status = RUNNING;

	gos_KeyboardFlush();
}

bool LogisticsScreen::setDefsListItems(const std::string& key, const std::vector<std::string>& items)
{
	if (!defsUiPage || !defsUiPage->isLoaded()) return false;
	return defsUiPage->setListItems(key, items);
}

bool LogisticsScreen::setDefsListItemColors(const std::string& key, const std::vector<unsigned int>& colors)
{
	if (!defsUiPage || !defsUiPage->isLoaded()) return false;
	return defsUiPage->setListItemColors(key, colors);
}

bool LogisticsScreen::getDefsElementScreenRect(const std::string& key, float& x, float& y, float& w, float& h)
{
	if (!defsUiPage || !defsUiPage->isLoaded()) return false;
	return defsUiPage->getElementScreenRect(key, x, y, w, h);
}

void LogisticsScreen::drawDefsRect(float x, float y, float w, float h, unsigned int color, bool filled)
{
	GuiRuntime::DrawUiRect(x, y, w, h, color, filled);
}

bool LogisticsScreen::setDefsSliderValue(const std::string& key, int value)
{
	if (!defsUiPage || !defsUiPage->isLoaded()) return false;
	return defsUiPage->setSliderValue(key, value);
}

int LogisticsScreen::getDefsSliderValue(const std::string& key) const
{
	if (!defsUiPage || !defsUiPage->isLoaded()) return 0;
	return defsUiPage->getSliderValue(key);
}

int LogisticsScreen::getDefsListItemCount(const std::string& key) const
{
	if (!defsUiPage || !defsUiPage->isLoaded()) return 0;
	return defsUiPage->getListItemCount(key);
}

int LogisticsScreen::getDefsListSelection(const std::string& key) const
{
	if (!defsUiPage || !defsUiPage->isLoaded()) return -1;
	return defsUiPage->getListSelection(key);
}

void LogisticsScreen::setDefsListSelection(const std::string& key, int index)
{
	if (defsUiPage && defsUiPage->isLoaded())
		defsUiPage->setListSelection(key, index);
}

bool LogisticsScreen::hasDefsEditBox() const
{
	return defsUiPage && defsUiPage->isLoaded() && defsUiPage->hasEditBox();
}

bool LogisticsScreen::getDefsEditText(const std::string& key, std::string& text) const
{
	if (!defsUiPage || !defsUiPage->isLoaded()) return false;
	return defsUiPage->getEditText(key, text);
}

bool LogisticsScreen::setDefsEditText(const std::string& key, const std::string& text)
{
	if (!defsUiPage || !defsUiPage->isLoaded()) return false;
	return defsUiPage->setEditText(key, text);
}

bool LogisticsScreen::isDefsEditBoxFocused(const std::string& key) const
{
	if (!defsUiPage || !defsUiPage->isLoaded()) return false;
	return defsUiPage->isEditBoxFocused(key);
}

bool LogisticsScreen::isAnyDefsEditBoxFocused() const
{
	if (!defsUiPage || !defsUiPage->isLoaded()) return false;
	return defsUiPage->isAnyEditBoxFocused();
}

void LogisticsScreen::requestDefsEditFocus(const std::string& key)
{
	if (defsUiPage && defsUiPage->isLoaded())
		defsUiPage->requestEditFocus(key);
}

bool LogisticsScreen::setDefsElementText(const std::string& key, const std::string& text)
{
	if (!defsUiPage || !defsUiPage->isLoaded()) return false;
	return defsUiPage->setElementText(key, text);
}

bool LogisticsScreen::setDefsElementVisible(const std::string& key, bool visible)
{
	if (!defsUiPage || !defsUiPage->isLoaded()) return false;
	return defsUiPage->setElementVisible(key, visible);
}

bool LogisticsScreen::setDefsElementTexture(const std::string& key, const std::string& texturePath)
{
	if (!defsUiPage || !defsUiPage->isLoaded()) return false;
	return defsUiPage->setElementTexture(key, texturePath);
}

bool LogisticsScreen::setDefsElementTextureNode(const std::string& key, long textureNode)
{
	if (!defsUiPage || !defsUiPage->isLoaded()) return false;
	return defsUiPage->setElementTextureNode(key, textureNode);
}

bool LogisticsScreen::setDefsElementGosTexture(const std::string& key, unsigned int gosHandle)
{
	if (!defsUiPage || !defsUiPage->isLoaded()) return false;
	return defsUiPage->setElementGosTexture(key, gosHandle);
}

bool LogisticsScreen::setDefsElementImageRegion(const std::string& key, const std::string& texturePath,
	int uvX, int uvY, int uvW, int uvH, int dstX, int dstY, int dstW, int dstH,
	bool legacyUvSpace)
{
	if (!defsUiPage || !defsUiPage->isLoaded()) return false;
	return defsUiPage->setElementImageRegion(key, texturePath, uvX, uvY, uvW, uvH, dstX, dstY, dstW, dstH, legacyUvSpace);
}


//*************************************************************************************************
// end of file ( LogisticsScreen.cpp )