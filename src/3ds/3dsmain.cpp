#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <vector>

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <3ds.h>

#include <dirent.h>

#include "3dstypes.h"
#include "3dsexit.h"
#include "3dsgpu.h"
#include "3dsopt.h"
#include "3dssound.h"
#include "3dsmenu.h"
#include "3dsui.h"
#include "3dsfont.h"
#include "3dsconfig.h"
#include "3dsfiles.h"
#include "3dsinput.h"
#include "3dslodepng.h"
#include "3dsmenu.h"
#include "3dsmain.h"
#include "3dsdbg.h"

#include "3dsinterface.h"
#include "3dscheat.h"
#include "3dsimpl_gpu.h"


SEmulator emulator;

int frameCount60 = 60;
u64 frameCountTick = 0;
int framesSkippedCount = 0;
char *romFileName = 0;
char romFileNameFullPath[_MAX_PATH];
char romFileNameLastSelected[_MAX_PATH];

u8* bottom_screen_buffer;
off_t bottom_screen_buffer_size;

u8* border_image_texture_buffer = NULL;
SGPUTexture *borderTexture;


//-------------------------------------------------------
// Clear top screen with logo.
//-------------------------------------------------------
void clearTopScreenWithLogo()
{
	unsigned char* image;
	unsigned width, height;

    int error = lodepng_decode32_file(&image, &width, &height, impl3dsTitleImage);

    if (!error && width == 400 && height == 240)
    {
        // lodepng outputs big endian rgba so we need to convert
        for (int i = 0; i < 2; i++)
        {
            u8* src = image;
            uint32* fb = (uint32 *) gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
            for (int y = 0; y < 240; y++)
                for (int x = 0; x < 400; x++)
                {
                    uint32 r = *src++;
                    uint32 g = *src++;
                    uint32 b = *src++;
                    uint32 a = *src++;

                    uint32 c = ((r << 24) | (g << 16) | (b << 8) | 0xff);
                    fb[x * 240 + (239 - y)] = c;
                }
            gfxSwapBuffers();
        }

        free(image);
    }
}

void renderBottomScreenImage()
{
    FILE *file = fopen("romfs:/bottom.bin", "rb");

    if (file)
    {
        gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
        gfxSetDoubleBuffering(GFX_BOTTOM, false);
        gfxSwapBuffersGpu();
    
        // seek to end of file
        fseek(file, 0, SEEK_END);
        // file pointer tells us the size
        bottom_screen_buffer_size = ftell(file);
        // seek back to start
        fseek(file, 0, SEEK_SET);

        if (bottom_screen_buffer != NULL) free(bottom_screen_buffer);

        //allocate a buffer
        bottom_screen_buffer = (u8*)(malloc(bottom_screen_buffer_size));
        //read contents !
        off_t bytesRead = fread(bottom_screen_buffer, 1, bottom_screen_buffer_size,file);
        //close the file because we like being nice and tidy
        fclose(file);

        //We don't need double buffering in this example. In this way we can draw our image only once on screen.
        gfxSetDoubleBuffering(GFX_BOTTOM, false);
        u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
        memcpy(fb, bottom_screen_buffer, bottom_screen_buffer_size);

        gfxFlushBuffers();
        gfxSwapBuffers();  
    }
    else
    {  
        // There's no bottom screen image, let's turn off the bottom screen
        turn_bottom_screen(TURN_OFF);
    }
}

void renderTopScreenBorder()
{
    // Copy the border texture  to the 3DS frame
    gpu3dsBindTexture(borderTexture, GPU_TEXUNIT0);
    gpu3dsSetTextureEnvironmentReplaceTexture0();
    gpu3dsDisableStencilTest();
    gpu3dsAddQuadVertexes(
        0,
        0,
        400,
        240,
        settings3DS.CropPixels,
        settings3DS.CropPixels,
        400 - settings3DS.CropPixels,
        240 - settings3DS.CropPixels,
        0.1f);

    gpu3dsDrawVertexes();
}

static u32 screen_next_pow_2(u32 i) {
    i--;
    i |= i >> 1;
    i |= i >> 2;
    i |= i >> 4;
    i |= i >> 8;
    i |= i >> 16;
    i++;

    return i;
}

void impl3dsClearBorderTexture()
{
    if (borderTexture == NULL) return;

    GX_MemoryFill(
        (u32*)borderTexture->PixelData, 0x00000000,
        (u32*)&((u8*)borderTexture->PixelData)[borderTexture->BufferSize],
        GX_FILL_TRIGGER | GX_FILL_32BIT_DEPTH,
        NULL, 0x00000000, NULL, 0);
}

bool impl3dsLoadBorderTexture(char *imgFilePath)
{
  unsigned char* src;
  unsigned width, height;
    int error = lodepng_decode32_file(&src, &width, &height, imgFilePath);
    if (!error && width == 400 && height == 240)
    {
      u32 pow2Width = screen_next_pow_2(width);
      u32 pow2Height = screen_next_pow_2(height);

      border_image_texture_buffer = (u8*)linearAlloc(pow2Width * pow2Height * 4);

      memset(border_image_texture_buffer, 0, pow2Width * pow2Height * 4);
      for(u32 x = 0; x < width; x++) {
          for(u32 y = 0; y < height; y++) {
              u32 dataPos = (y * width + x) * 4;
              u32 pow2TexPos = (y * pow2Width + x) * 4;

              border_image_texture_buffer[pow2TexPos + 0] = ((u8*) src)[dataPos + 3];
              border_image_texture_buffer[pow2TexPos + 1] = ((u8*) src)[dataPos + 2];
              border_image_texture_buffer[pow2TexPos + 2] = ((u8*) src)[dataPos + 1];
              border_image_texture_buffer[pow2TexPos + 3] = ((u8*) src)[dataPos + 0];
          }
      }
      
      GSPGPU_FlushDataCache(border_image_texture_buffer, pow2Width * pow2Height * 4);

      borderTexture = gpu3dsCreateTextureInVRAM(pow2Width, pow2Height, GPU_RGBA8);

      GX_DisplayTransfer((u32*)border_image_texture_buffer,GX_BUFFER_DIM(pow2Width, pow2Height),(u32*)borderTexture->PixelData,GX_BUFFER_DIM(pow2Width, pow2Height),
      GX_TRANSFER_FLIP_VERT(1) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GPU_RGBA8) |
      GX_TRANSFER_OUT_FORMAT((u32) GPU_RGBA8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));

      gspWaitForPPF();
      
      free(src);
      return true;
    }
  return false;
}

//----------------------------------------------------------------------
// Start up menu.
//----------------------------------------------------------------------
SMenuItem emulatorNewMenu[] = {
    MENU_MAKE_ACTION(6001, "  Exit"),
    MENU_MAKE_LASTITEM  ()
    };

extern SMenuItem emulatorMenu[];


//-------------------------------------------------------
// Load the ROM and reset the CPU.
//-------------------------------------------------------

bool emulatorSettingsLoad(bool, bool, bool);
bool emulatorSettingsSave(bool, bool, bool);

bool emulatorLoadRom()
{
    //menu3dsShowDialog("Load ROM", "Loading... this may take a while.", DIALOGCOLOR_CYAN, NULL);

    //emulatorSettingsSave(true, true, false);
    
    char romFileNameFullPath2[_MAX_PATH];
    strncpy(romFileNameFullPath2, romFileNameFullPath, _MAX_PATH - 1);

    FILE* path_fp = fopen("romfs:/rom_path.txt", "r");
    if (!path_fp)
    {
        exit(0);
    }
    else
    {
        fgets(romFileNameFullPath2, sizeof(romFileNameFullPath2), path_fp);
    }

    fclose(path_fp);

    // Load up the new ROM settings first.
    //
    emulatorSettingsLoad(false, true, false);
    impl3dsApplyAllSettings();
    
    if (!impl3dsLoadROM(romFileNameFullPath2))
    {
        exit(0);
    }
    impl3dsApplyAllSettings();

    if (settings3DS.AutoSavestate)
        impl3dsLoadState(0);

    emulator.emulatorState = EMUSTATE_EMULATE;

    cheat3dsLoadCheatTextFile(file3dsReplaceFilenameExtension(romFileNameFullPath, ".chx"));
    //menu3dsHideDialog();

    // Fix: Game-specific settings that never get saved.
    impl3dsCopyMenuToOrFromSettings(false);

    return true;
}


//----------------------------------------------------------------------
// Menus
//----------------------------------------------------------------------
#define MAX_FILES 1000
SMenuItem fileMenu[MAX_FILES + 1];
//char romFileNames[MAX_FILES][_MAX_PATH];
// By changing the romFileNames to fileList (strings allocated on demand)
// this fixes the crashing problem on Old 3DS when running Luma 8 and Rosalina 2.
// 
std::vector<std::string> fileList;

int totalRomFileCount = 0;

//----------------------------------------------------------------------
// Load all ROM file names (up to 1000 ROMs)
//----------------------------------------------------------------------
void fileGetAllFiles(void)
{
    fileList = file3dsGetFiles(impl3dsRomExtensions, MAX_FILES);

    totalRomFileCount = 0;

    // Increase the total number of files we can display.
    for (int i = 0; i < fileList.size() && i < MAX_FILES; i++)
    {
        //strncpy(romFileNames[i], fileList[i].c_str(), _MAX_PATH);
        totalRomFileCount++;
        fileMenu[i].Type = MENUITEM_ACTION;
        fileMenu[i].ID = i;
        fileMenu[i].Text = fileList[i].c_str();
    }
    fileMenu[totalRomFileCount].Type = MENUITEM_LASTITEM;
}


//----------------------------------------------------------------------
// Find the ID of the last selected file in the file list.
//----------------------------------------------------------------------
int fileFindLastSelectedFile()
{
    for (int i = 0; i < totalRomFileCount && i < MAX_FILES; i++)
    {
        if (strncmp(fileMenu[i].Text, romFileNameLastSelected, _MAX_PATH) == 0)
            return i;
    }
    return -1;
}


//----------------------------------------------------------------------
// Load global settings, and game-specific settings.
//----------------------------------------------------------------------
bool emulatorSettingsLoad(bool includeGlobalSettings, bool includeGameSettings, bool showMessage = true)
{
    if (includeGlobalSettings)
    {
        bool success = impl3dsReadWriteSettingsGlobal(false);
        if (success)
        {
            input3dsSetDefaultButtonMappings(settings3DS.GlobalButtonMapping, settings3DS.GlobalTurbo, false);
            impl3dsApplyAllSettings(false);
        }
        else
        {
            impl3dsInitializeDefaultSettingsGlobal();
            input3dsSetDefaultButtonMappings(settings3DS.GlobalButtonMapping, settings3DS.GlobalTurbo, true);
            impl3dsApplyAllSettings(false);
            return false;
        }
    }

    if (includeGameSettings)
    {
        bool success = impl3dsReadWriteSettingsByGame(false);
        if (success)
        {
            input3dsSetDefaultButtonMappings(settings3DS.ButtonMapping, settings3DS.Turbo, false);
            impl3dsApplyAllSettings();
            return true;
        }
        else
        {
            impl3dsInitializeDefaultSettingsByGame();
            input3dsSetDefaultButtonMappings(settings3DS.ButtonMapping, settings3DS.Turbo, true);
            impl3dsApplyAllSettings();

            //return emulatorSettingsSave(true, showMessage);
            return true;
        }
    }
    return true;
}


//----------------------------------------------------------------------
// Save global settings, and game-specific settings.
//----------------------------------------------------------------------
bool emulatorSettingsSave(bool includeGlobalSettings, bool includeGameSettings, bool showMessage)
{
    if (showMessage)
    {
        //consoleClear();
        //ui3dsDrawRect(50, 140, 270, 154, 0x000000);
        //ui3dsDrawStringWithNoWrapping(50, 140, 270, 154, 0x3f7fff, HALIGN_CENTER, "Saving settings to SD card...");
    }
    /*
    if (includeGameSettings)
    {
        impl3dsReadWriteSettingsByGame(true);
    }

    if (includeGlobalSettings)
    {
        impl3dsReadWriteSettingsGlobal(true);
    }
    */
    if (showMessage)
    {
        //ui3dsDrawRect(50, 140, 270, 154, 0x000000);
    }

    return true;
}



//----------------------------------------------------------------------
// Checks if file exists.
//----------------------------------------------------------------------
bool IsFileExists(const char * filename) {
    if (FILE * file = fopen(filename, "r")) {
        fclose(file);
        return true;
    }
    return false;
}


//----------------------------------------------------------------------
// Menu when the emulator is paused in-game.
//----------------------------------------------------------------------
bool menuSelectedChanged(int ID, int value)
{
    if (ID >= 50000 && ID <= 51000)
    {
        // Handle cheats
        int enabled = menu3dsGetValueByID(-1, ID);
        impl3dsSetCheatEnabledFlag(ID - 50000, enabled == 1);
        cheat3dsSetCheatEnabledFlag(ID - 50000, enabled == 1);
        return false;
    }

    return impl3dsOnMenuSelectedChanged(ID, value);
}


//----------------------------------------------------------------------
// Menu when the emulator is paused in-game.
//----------------------------------------------------------------------
void menuPause()
{
    // Let's turn on the bottom screen just in case it's turned off
    turn_bottom_screen(TURN_ON);

    gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES);
    gfxSwapBuffersGpu();
    menu3dsDrawBlackScreen();

    gfxSetDoubleBuffering(GFX_BOTTOM, true);
    
    bool settingsUpdated = false;
    bool cheatsUpdated = false;
    bool settingsSaved = false;
    bool returnToEmulation = false;


    menu3dsClearMenuTabs();
    menu3dsAddTab("Game", emulatorMenu);
    //menu3dsAddTab("Options", optionMenu);
    //menu3dsAddTab("Controls", controlsMenu);
    menu3dsAddTab("Cheats", cheatMenu);
    //menu3dsAddTab("Select ROM", fileMenu);

    //impl3dsCopyMenuToOrFromSettings(false);

    //int previousFileID = fileFindLastSelectedFile();
    menu3dsSetTabSubTitle(0, NULL);
    menu3dsSetTabSubTitle(1, NULL);
    //menu3dsSetTabSubTitle(2, NULL);
    //menu3dsSetTabSubTitle(3, NULL);
    //menu3dsSetTabSubTitle(4, file3dsGetCurrentDir());
    //if (previousFileID >= 0)
    //    menu3dsSetSelectedItemIndexByID(4, previousFileID);
    menu3dsSetCurrentMenuTab(0);
    menu3dsSetTransferGameScreen(true);

    bool animateMenu = true;

    while (true)
    {
        if (appExiting)
        {
            break;
        }

        int selection = menu3dsShowMenu(menuSelectedChanged, animateMenu);
        animateMenu = false;

        if (selection == -1 || selection == 1000)
        {
            // Cancels the menu and resumes game
            //
            returnToEmulation = true;

            break;
        }
        else if (selection < 1000)
        {
            // Load ROM
            //
            //romFileName = romFileNames[selection];
            romFileName = fileList[selection].c_str();
            if (romFileName[0] == 1)
            {
                if (strcmp(romFileName, "\x01 ..") == 0)
                    file3dsGoToParentDirectory();
                else
                    file3dsGoToChildDirectory(&romFileName[2]);

                fileGetAllFiles();
                menu3dsClearMenuTabs();
                menu3dsAddTab("Game", emulatorMenu);
                //menu3dsAddTab("Options", optionMenu);
                //menu3dsAddTab("Controls", controlsMenu);
                menu3dsAddTab("Cheats", cheatMenu);
                //menu3dsAddTab("Select ROM", fileMenu);
                menu3dsSetCurrentMenuTab(0);
                //menu3dsSetTabSubTitle(4, file3dsGetCurrentDir());
            }
            else
            {
                strncpy(romFileNameLastSelected, romFileName, _MAX_PATH);

                bool loadRom = true;
                if (settings3DS.AutoSavestate) {
                    menu3dsShowDialog("Save State", "Autosaving state...", DIALOGCOLOR_RED, NULL);
                    bool result = impl3dsSaveState(0);
                    menu3dsHideDialog();

                    if (!result) {
                        int choice = menu3dsShowDialog("Autosave failure", "Automatic savestate writing failed.\nLoad chosen game anyway?", DIALOGCOLOR_RED, optionsForNoYes);
                        if (choice != 1) {
                            loadRom = false;
                        }
                    }
                }

                if (loadRom)
                {
                    // Save settings and cheats, before loading
                    // your new ROM.
                    //
                    if (impl3dsCopyMenuToOrFromSettings(true))
                    {
                        emulatorSettingsSave(true, true, false);
                    }
                    else
                    {
                        emulatorSettingsSave(true, false, false);
                    }
                    settingsSaved = true;

                    if (!emulatorLoadRom())
                    {
                        menu3dsShowDialog("Load ROM", "Hmm... unable to load ROM.", DIALOGCOLOR_RED, optionsForOk);
                        menu3dsHideDialog();
                    }
                    else
                        break;
                }
            }
        }
        else if (selection >= 2001 && selection <= 2010)
        {
            int slot = selection - 2000;
            char text[200];
           
            sprintf(text, "Saving into slot %d...\nThis may take a while", slot);
            menu3dsShowDialog("Savestates", text, DIALOGCOLOR_CYAN, NULL);
            bool result = impl3dsSaveState(slot);
            menu3dsHideDialog();

            if (result)
            {
                sprintf(text, "Slot %d save completed.", slot);
                result = menu3dsShowDialog("Savestates", text, DIALOGCOLOR_GREEN, optionsForOk);
                menu3dsHideDialog();
            }
            else
            {
                sprintf(text, "Oops. Unable to save slot %d!", slot);
                result = menu3dsShowDialog("Savestates", text, DIALOGCOLOR_RED, optionsForOk);
                menu3dsHideDialog();
            }

            menu3dsSetSelectedItemIndexByID(0, 1000);
        }
        else if (selection >= 3001 && selection <= 3010)
        {
            int slot = selection - 3000;
            char text[200];

            bool result = impl3dsLoadState(slot);
            if (result)
            {
                returnToEmulation = true;
                break;
            }
            else
            {
                sprintf(text, "Oops. Unable to load slot %d!", slot);
                menu3dsShowDialog("Savestates", text, DIALOGCOLOR_RED, optionsForOk);
                menu3dsHideDialog();
            }
        }
        else if (selection == 4001)
        {
            menu3dsShowDialog("Screenshot", "Now taking a screenshot...\nThis may take a while.", DIALOGCOLOR_CYAN, NULL);

            char ext[256];
            const char *path = NULL;

            // Loop through and look for an non-existing
            // file name.
            //
            int i = 1;
            while (i <= 999)
            {
                snprintf(ext, 255, ".b%03d.bmp", i);
                path = file3dsReplaceFilenameExtension(romFileNameFullPath, ext);
                if (!IsFileExists(path))
                    break;
                path = NULL;
                i++;
            }

            bool success = false;
            if (path)
            {
                success = menu3dsTakeScreenshot(path);
            }
            menu3dsHideDialog();

            if (success)
            {
                char text[600];
                snprintf(text, 600, "Done! File saved to %s", path);
                menu3dsShowDialog("Screenshot", text, DIALOGCOLOR_GREEN, optionsForOk);
                menu3dsHideDialog();
            }
            else 
            {
                menu3dsShowDialog("Screenshot", "Oops. Unable to take screenshot!", DIALOGCOLOR_RED, optionsForOk);
                menu3dsHideDialog();
            }
        }
        else if (selection == 5001)
        {
            int result = menu3dsShowDialog("Reset Console", "Are you sure?", DIALOGCOLOR_RED, optionsForNoYes);
            menu3dsHideDialog();

            if (result == 1)
            {
                impl3dsResetConsole();
                returnToEmulation = true;
                break;
            }
            
        }
        else if (selection == 6001)
        {
            int result = menu3dsShowDialog("Exit",  "Leaving so soon?", DIALOGCOLOR_RED, optionsForNoYes);
            if (result == 1)
            {
                emulator.emulatorState = EMUSTATE_END;

                break;
            }
            else
                menu3dsHideDialog();
            
        }
        else
        {
            bool endMenu = impl3dsOnMenuSelected(selection);
            if (endMenu)
            {
                returnToEmulation = true;
                break;
            }
        }

    }

    menu3dsHideMenu();

    cheat3dsSaveCheatTextFile (file3dsReplaceFilenameExtension(romFileNameFullPath, ".chx"));

    if (returnToEmulation)
    {
        emulator.emulatorState = EMUSTATE_EMULATE;
        renderBottomScreenImage();
    }

    // Loads the new ROM if a ROM was selected.
    //
    //if (loadRomBeforeExit)
    //    emulatorLoadRom();

}

//-------------------------------------------------------
// Sets up all the cheats to be displayed in the menu.
//-------------------------------------------------------
SMenuItem cheatMenu[401] =
{
    MENU_MAKE_HEADER2   ("Cheats"),
    MENU_MAKE_LASTITEM  ()
};


char *noCheatsText[] {
    "",
    "    No cheats available for this game ",
    "",
    "    To enable cheats:  ",
    "      Copy your file into the same folder as  ",
    "      ROM file and make sure it has the same name. ",
    "",
    "      If your ROM filename is: ",
    "          MyGame.abc",
    "      Then your cheat filename must be: ",
    "          MyGame.CHX",
    "",
    "    Refer to readme.md for the .CHX file format. ",
    ""
     };


//--------------------------------------------------------
// Initialize the emulator engine and everything else.
// This calls the impl3dsInitializeCore, which executes
// initialization code specific to the emulation core.
//--------------------------------------------------------
void emulatorInitialize()
{
    emulator.enableDebug = false;
    emulator.emulatorState = 0;
    emulator.waitBehavior = 0;

    file3dsInitialize();

    romFileNameLastSelected[0] = 0;

    if (!gpu3dsInitialize())
    {
        printf ("Unable to initialize GPU\n");
        exit(0);
    }

    printf ("Initializing...\n");

    if (!impl3dsInitializeCore())
    {
        printf ("Unable to initialize emulator core\n");
        exit(0);
    }

    if (!snd3dsInitialize())
    {
        printf ("Unable to initialize CSND\n");
        exit (0);
    }

    ui3dsInitialize();

    if (romfsInit()!=0)
    {
        printf ("Unable to initialize romfs\n");
        exit(0);
    }

    FILE* path_fp = fopen("romfs:/internal_name.txt", "r");
    if (!path_fp)
        exit(0);

    fgets(internalName, sizeof(internalName), path_fp);

    if (!impl3dsLoadBorderTexture("romfs:/border.png"))
        borderTexture = gpu3dsCreateTextureInVRAM(400, 240, GPU_RGBA8);
    
    printf ("Initialization complete\n");

    osSetSpeedupEnable(1);    // Performance: use the higher clock speed for new 3DS.

    enableAptHooks();

    emulatorSettingsLoad(true, false, true);

    // Do this one more time.
    if (file3dsGetCurrentDir()[0] == 0)
        file3dsInitialize();
}


//--------------------------------------------------------
// Finalize the emulator.
//--------------------------------------------------------
void emulatorFinalize()
{
    free(bottom_screen_buffer);
    linearFree(border_image_texture_buffer);
    if (borderTexture != NULL) gpu3dsDestroyTextureFromVRAM(borderTexture);
    
    consoleClear();

    impl3dsFinalize();

#ifndef EMU_RELEASE
    printf("gspWaitForP3D:\n");
#endif
    gspWaitForVBlank();
    gpu3dsWaitForPreviousFlush();
    gspWaitForVBlank();

#ifndef EMU_RELEASE
    printf("snd3dsFinalize:\n");
#endif
    snd3dsFinalize();

#ifndef EMU_RELEASE
    printf("gpu3dsFinalize:\n");
#endif
    gpu3dsFinalize();

#ifndef EMU_RELEASE
    printf("ptmSysmExit:\n");
#endif
    ptmSysmExit ();

    printf("romfsExit:\n");
    romfsExit();
    
#ifndef EMU_RELEASE
    printf("hidExit:\n");
#endif
	hidExit();
    
#ifndef EMU_RELEASE
    printf("aptExit:\n");
#endif
	aptExit();
/*    
#ifndef EMU_RELEASE
    printf("srvExit:\n");
#endif
	srvExit();
*/
}



bool firstFrame = true;


//---------------------------------------------------------
// Counts the number of frames per second, and prints
// it to the bottom screen every 60 frames.
//---------------------------------------------------------
char frameCountBuffer[70];
void updateFrameCount()
{
    if (frameCountTick == 0)
        frameCountTick = svcGetSystemTick();

    if (frameCount60 == 0)
    {
        u64 newTick = svcGetSystemTick();
        float timeDelta = ((float)(newTick - frameCountTick))/TICKS_PER_SEC;
        int fpsmul10 = (int)((float)600 / timeDelta);

#if !defined(EMU_RELEASE) && !defined(DEBUG_CPU) && !defined(DEBUG_APU)
        //consoleClear();
#endif
        /*
        if (settings3DS.HideUnnecessaryBottomScrText == 0)
        {
            if (framesSkippedCount)
                snprintf (frameCountBuffer, 69, "FPS: %2d.%1d (%d skipped)\n", fpsmul10 / 10, fpsmul10 % 10, framesSkippedCount);
            else
                snprintf (frameCountBuffer, 69, "FPS: %2d.%1d \n", fpsmul10 / 10, fpsmul10 % 10);

            ui3dsDrawRect(2, 2, 200, 16, 0x000000);
            ui3dsDrawStringWithNoWrapping(2, 2, 200, 16, 0x7f7f7f, HALIGN_LEFT, frameCountBuffer);
        }
        */

        frameCount60 = 60;
        framesSkippedCount = 0;


#if !defined(EMU_RELEASE) && !defined(DEBUG_CPU) && !defined(DEBUG_APU)
        printf ("\n\n");
        for (int i=0; i<100; i++)
        {
            t3dsShowTotalTiming(i);
        }
        t3dsResetTimings();
#endif
        frameCountTick = newTick;

    }

    frameCount60--;
}





//----------------------------------------------------------
// This is the main emulation loop. It calls the 
//    impl3dsRunOneFrame
//   (which must be implemented for any new core)
// for the execution of the frame.
//----------------------------------------------------------
void emulatorLoop()
{
	// Main loop
    //emulator.enableDebug = true;
    emulator.waitBehavior = WAIT_FULL;

    int emuFramesSkipped = 0;
    long emuFrameTotalActualTicks = 0;
    long emuFrameTotalAccurateTicks = 0;

    bool firstFrame = true;
    appSuspended = 0;

    gpu3dsResetState();

    frameCount60 = 60;
    frameCountTick = 0;
    framesSkippedCount = 0;

    long startFrameTick = svcGetSystemTick();

    bool skipDrawingFrame = false;

    // Reinitialize the console.
    //renderBottomScreenImage();
    //consoleInit(GFX_BOTTOM, NULL);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    //menu3dsDrawBlackScreen();
    /*
    if (settings3DS.HideUnnecessaryBottomScrText == 0)
    {
        ui3dsDrawStringWithNoWrapping(0, 100, 320, 115, 0x7f7f7f, HALIGN_CENTER, "Touch screen for menu");
    }
    */

    snd3dsStartPlaying();

    impl3dsEmulationBegin();

	while (true)
	{
        startFrameTick = svcGetSystemTick();
        aptMainLoop();

        if (appExiting || appSuspended)
            break;

        gpu3dsStartNewFrame();
        //gpu3dsCheckSlider();
        updateFrameCount();

    	input3dsScanInputForEmulation();
        if (emulator.emulatorState != EMUSTATE_EMULATE)
            break;

        impl3dsEmulationRunOneFrame(firstFrame, skipDrawingFrame);

        firstFrame = false; 

        // This either waits for the next frame, or decides to skip
        // the rendering for the next frame if we are too slow.
        //
#ifndef EMU_RELEASE
        if (emulator.isReal3DS)
#endif
        {

            // Check the keys to see if the user is fast-forwarding
            //
            int keysHeld = input3dsGetCurrentKeysHeld();
            emulator.fastForwarding = false;
            if ((settings3DS.UseGlobalEmuControlKeys && (settings3DS.GlobalButtonHotkeyDisableFramelimit & keysHeld)) ||
                (!settings3DS.UseGlobalEmuControlKeys && (settings3DS.ButtonHotkeyDisableFramelimit & keysHeld))) 
                emulator.fastForwarding = true;

            long currentTick = svcGetSystemTick();
            long actualTicksThisFrame = currentTick - startFrameTick;
            long ticksPerFrame = settings3DS.TicksPerFrame;
            if (emulator.fastForwarding)
                ticksPerFrame = TICKS_PER_FRAME_FASTFORWARD;

            emuFrameTotalActualTicks += actualTicksThisFrame;  // actual time spent rendering past x frames.
            emuFrameTotalAccurateTicks += ticksPerFrame;  // time supposed to be spent rendering past x frames.

            int isSlow = 0;

            long skew = emuFrameTotalAccurateTicks - emuFrameTotalActualTicks;

            if (skew < 0)
            {
                // We've skewed out of the actual frame rate.
                // Once we skew beyond 0.1 (10%) frames slower, skip the frame.
                //
                if (skew < -ticksPerFrame/10 && emuFramesSkipped < settings3DS.MaxFrameSkips)
                {
                    skipDrawingFrame = true;
                    emuFramesSkipped++;

                    framesSkippedCount++;   // this is used for the stats display every 60 frames.
                }
                else
                {
                    skipDrawingFrame = false;

                    if (emuFramesSkipped >= settings3DS.MaxFrameSkips)
                    {
                        emuFramesSkipped = 0;
                        emuFrameTotalActualTicks = actualTicksThisFrame;
                        emuFrameTotalAccurateTicks = ticksPerFrame;
                    }
                }
            }
            else
            {

                float timeDiffInMilliseconds = (float)skew * 1000000 / TICKS_PER_SEC;
                if (emulator.waitBehavior == WAIT_HALF)
                    timeDiffInMilliseconds /= 2;
                else if (emulator.waitBehavior == WAIT_NONE)
                    timeDiffInMilliseconds = 1;
                emulator.waitBehavior = WAIT_FULL;

                // Reset the counters.
                //
                emuFrameTotalActualTicks = 0;
                emuFrameTotalAccurateTicks = 0;
                emuFramesSkipped = 0;

                svcSleepThread ((long)(timeDiffInMilliseconds * 1000));
                skipDrawingFrame = false;
            }

        }

	}

    snd3dsStopPlaying();

    // Wait for the sound thread to leave the snd3dsMixSamples entirely
    // to prevent a race condition between the PTMU_GetBatteryChargeState (when
    // drawing the menu) and GSPGPU_FlushDataCache (in the sound thread).
    //
    // (There's probably a better way to do this, but this will do for now)
    //
    svcSleepThread(500000);
}


//---------------------------------------------------------
// Main entrypoint.
//---------------------------------------------------------
int main()
{
    mkdir("sdmc:/nsui_forwarders_data", 0777);

    emulatorInitialize();

    static char s[PATH_MAX + 1];
    snprintf(s, PATH_MAX + 1, "sdmc:/nsui_forwarders_data/%s", internalName);
    mkdir(s, 0777);

    //clearTopScreenWithLogo();
    
    emulatorLoadRom();

    renderBottomScreenImage();

    while (true)
    {
        if (appExiting)
            goto quit;

        switch (emulator.emulatorState)
        {
            case EMUSTATE_PAUSEMENU:
                menuPause();
                break;

            case EMUSTATE_EMULATE:
                emulatorLoop();
                break;

            case EMUSTATE_END:
                goto quit;

        }

    }

quit:
    if (emulator.emulatorState > 0 && settings3DS.AutoSavestate)
        impl3dsSaveState(0);

    printf("emulatorFinalize:\n");
    emulatorFinalize();
    printf ("Exiting...\n");
	exit(0);
}
