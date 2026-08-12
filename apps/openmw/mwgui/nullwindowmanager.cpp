#include "nullwindowmanager.hpp"

#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>

namespace MWGui
{
    NullWindowManager::NullWindowManager(std::string versionDescription, std::function<void()> requestQuit)
        : mVersionDescription(std::move(versionDescription))
        , mRequestQuit(std::move(requestQuit))
    {
    }

#define OMW_NULL_VOID(name, signature) void NullWindowManager::name signature {}

    OMW_NULL_VOID(playVideo, (std::string_view, bool, bool))
    OMW_NULL_VOID(setNewGame, (bool))
    OMW_NULL_VOID(pushGuiMode, (GuiMode, const MWWorld::Ptr&))
    OMW_NULL_VOID(pushGuiMode, (GuiMode))
    OMW_NULL_VOID(popGuiMode, (bool))
    OMW_NULL_VOID(removeGuiMode, (GuiMode))
    OMW_NULL_VOID(goToJail, (int))
    OMW_NULL_VOID(updatePlayer, ())
    OMW_NULL_VOID(toggleVisible, (GuiWindow))
    OMW_NULL_VOID(forceHide, (GuiWindow))
    OMW_NULL_VOID(unsetForceHide, (GuiWindow))
    OMW_NULL_VOID(disallowAll, ())
    OMW_NULL_VOID(allow, (GuiWindow))
    OMW_NULL_VOID(useItem, (const MWWorld::Ptr&, bool))
    OMW_NULL_VOID(updateSpellWindow, ())
    OMW_NULL_VOID(setConsoleSelectedObject, (const MWWorld::Ptr&))
    OMW_NULL_VOID(setConsoleMode, (std::string_view))
    OMW_NULL_VOID(printToConsole, (const std::string&, std::string_view))
    OMW_NULL_VOID(setDrowningTimeLeft, (float, float))
    OMW_NULL_VOID(changeCell, (const MWWorld::CellStore*))
    OMW_NULL_VOID(setFocusObject, (const MWWorld::Ptr&))
    OMW_NULL_VOID(setFocusObjectScreenCoords, (float, float))
    OMW_NULL_VOID(setCursorActive, (bool))
    OMW_NULL_VOID(setDragDrop, (bool))
    OMW_NULL_VOID(setDrowningBarVisibility, (bool))
    OMW_NULL_VOID(setHMSVisibility, (bool))
    OMW_NULL_VOID(setMinimapVisibility, (bool))
    OMW_NULL_VOID(setWeaponVisibility, (bool))
    OMW_NULL_VOID(setSpellVisibility, (bool))
    OMW_NULL_VOID(setSneakVisibility, (bool))
    OMW_NULL_VOID(activateQuickKey, (int))
    OMW_NULL_VOID(updateActivatedQuickKey, ())
    OMW_NULL_VOID(setSelectedSpell, (const ESM::RefId&, int))
    OMW_NULL_VOID(setSelectedEnchantItem, (const MWWorld::Ptr&))
    OMW_NULL_VOID(setSelectedWeapon, (const MWWorld::Ptr&))
    OMW_NULL_VOID(unsetSelectedSpell, ())
    OMW_NULL_VOID(unsetSelectedWeapon, ())
    OMW_NULL_VOID(showCrosshair, (bool))
    OMW_NULL_VOID(disallowMouse, ())
    OMW_NULL_VOID(allowMouse, ())
    OMW_NULL_VOID(notifyInputActionBound, ())
    OMW_NULL_VOID(addVisitedLocation, (const std::string&, int, int))
    OMW_NULL_VOID(removeDialog, (std::unique_ptr<MWGui::Layout>&&))
    OMW_NULL_VOID(exitCurrentGuiMode, ())
    OMW_NULL_VOID(messageBox, (std::string_view, MWGui::ShowInDialogueMode))
    OMW_NULL_VOID(scheduleMessageBox, (std::string, MWGui::ShowInDialogueMode))
    OMW_NULL_VOID(staticMessageBox, (std::string_view))
    OMW_NULL_VOID(removeStaticMessageBox, ())
    OMW_NULL_VOID(interactiveMessageBox, (std::string_view, const std::vector<std::string>&, bool, int))
    OMW_NULL_VOID(updateConsoleObjectPtr, (const MWWorld::Ptr&, const MWWorld::Ptr&))
    OMW_NULL_VOID(processChangedSettings, (const std::set<std::pair<std::string, std::string>>&))
    OMW_NULL_VOID(executeInConsole, (const std::filesystem::path&))
    OMW_NULL_VOID(enableRest, ())
    OMW_NULL_VOID(wakeUpPlayer, ())
    OMW_NULL_VOID(showSoulgemDialog, (MWWorld::Ptr))
    OMW_NULL_VOID(changePointer, (const std::string&))
    OMW_NULL_VOID(setEnemy, (const MWWorld::Ptr&))
    OMW_NULL_VOID(setKeyFocusWidget, (MyGUI::Widget*))
    OMW_NULL_VOID(clear, ())
    OMW_NULL_VOID(write, (ESM::ESMWriter&, Loading::Listener&))
    OMW_NULL_VOID(readRecord, (ESM::ESMReader&, uint32_t))
    OMW_NULL_VOID(exitCurrentModal, ())
    OMW_NULL_VOID(addCurrentModal, (MWGui::WindowModal*))
    OMW_NULL_VOID(removeCurrentModal, (MWGui::WindowModal*))
    OMW_NULL_VOID(pinWindow, (GuiWindow))
    OMW_NULL_VOID(toggleMaximized, (MWGui::Layout*))
    OMW_NULL_VOID(fadeScreenIn, (float, bool, float))
    OMW_NULL_VOID(fadeScreenOut, (float, bool, float))
    OMW_NULL_VOID(fadeScreenTo, (int, float, bool, float))
    OMW_NULL_VOID(setBlindness, (int))
    OMW_NULL_VOID(activateHitOverlay, (bool))
    OMW_NULL_VOID(setWerewolfOverlay, (bool))
    OMW_NULL_VOID(toggleConsole, ())
    OMW_NULL_VOID(toggleDebugWindow, ())
    OMW_NULL_VOID(togglePostProcessorHud, ())
    OMW_NULL_VOID(toggleSettingsWindow, ())
    OMW_NULL_VOID(cycleSpell, (bool))
    OMW_NULL_VOID(cycleWeapon, (bool))
    OMW_NULL_VOID(playSound, (const ESM::RefId&, float, float))
    OMW_NULL_VOID(addCell, (MWWorld::CellStore*))
    OMW_NULL_VOID(removeCell, (MWWorld::CellStore*))
    OMW_NULL_VOID(writeFog, (MWWorld::CellStore*))
    OMW_NULL_VOID(windowResized, (int, int))
    OMW_NULL_VOID(watchActor, (const MWWorld::Ptr&))
    OMW_NULL_VOID(onDeleteCustomData, (const MWWorld::Ptr&))
    OMW_NULL_VOID(forceLootMode, (const MWWorld::Ptr&))
    OMW_NULL_VOID(asyncPrepareSaveMap, ())
    OMW_NULL_VOID(setCullMask, (uint32_t))
    OMW_NULL_VOID(inventoryUpdated, (const MWWorld::Ptr&) const)
    OMW_NULL_VOID(cycleActiveControllerWindow, (bool))
    OMW_NULL_VOID(setActiveControllerWindow, (GuiMode, size_t))
    OMW_NULL_VOID(setControllerTooltipVisible, (bool))
    OMW_NULL_VOID(setControllerTooltipEnabled, (bool))
    OMW_NULL_VOID(restoreControllerTooltips, ())
    OMW_NULL_VOID(updateControllerButtonsOverlay, ())
    OMW_NULL_VOID(setDisabledByLua, (std::string_view, bool))

#undef OMW_NULL_VOID

    GuiMode NullWindowManager::getMode() const { return GM_None; }
    bool NullWindowManager::containsMode(GuiMode) const { return false; }
    bool NullWindowManager::isGuiMode() const { return false; }
    bool NullWindowManager::isConsoleMode() const { return false; }
    bool NullWindowManager::isPostProcessorHudVisible() const { return false; }
    bool NullWindowManager::isSettingsWindowVisible() const { return false; }
    bool NullWindowManager::isInteractiveMessageBoxActive() const { return false; }
    bool NullWindowManager::isAllowed(GuiWindow) const { return false; }
    MWGui::InventoryWindow* NullWindowManager::getInventoryWindow() { return nullptr; }
    MWGui::CountDialog* NullWindowManager::getCountDialog() { return nullptr; }
    MWGui::ConfirmationDialog* NullWindowManager::getConfirmationDialog() { return nullptr; }
    MWGui::TradeWindow* NullWindowManager::getTradeWindow() { return nullptr; }
    MWGui::HUD* NullWindowManager::getHud() { return nullptr; }
    MWGui::PostProcessorHud* NullWindowManager::getPostProcessorHud() { return nullptr; }
    std::vector<MWGui::WindowBase*> NullWindowManager::getGuiModeWindows(GuiMode) { return {}; }
    MWWorld::Ptr NullWindowManager::getConsoleSelectedObject() const { return mEmptyPtr; }
    const std::string& NullWindowManager::getConsoleMode()
    {
        static const std::string empty;
        return empty;
    }
    void NullWindowManager::setCursorVisible(bool visible) { mCursorVisible = visible; }
    void NullWindowManager::getMousePosition(int& x, int& y) { x = 0; y = 0; }
    void NullWindowManager::getMousePosition(float& x, float& y) { x = 0.f; y = 0.f; }
    bool NullWindowManager::getWorldMouseOver() { return false; }
    float NullWindowManager::getScalingFactor() const { return 1.f; }
    bool NullWindowManager::toggleFogOfWar() { return false; }
    bool NullWindowManager::toggleFullHelp() { return false; }
    bool NullWindowManager::getFullHelp() const { return false; }
    const ESM::RefId& NullWindowManager::getSelectedSpell()
    {
        static const ESM::RefId empty;
        return empty;
    }
    const MWWorld::Ptr& NullWindowManager::getSelectedEnchantItem() const { return mEmptyPtr; }
    const MWWorld::Ptr& NullWindowManager::getSelectedWeapon() const { return mEmptyPtr; }
    bool NullWindowManager::setHudVisibility(bool visible) { mHudVisible = visible; return true; }
    bool NullWindowManager::isHudVisible() const { return mHudVisible; }
    int NullWindowManager::readPressedButton() { return -1; }
    std::string_view NullWindowManager::getGameSettingString(std::string_view, std::string_view defaultValue)
    {
        return defaultValue;
    }
    bool NullWindowManager::getRestEnabled() { return false; }
    bool NullWindowManager::getJournalAllowed() { return false; }
    bool NullWindowManager::getPlayerSleeping() { return false; }
    std::size_t NullWindowManager::getMessagesCount() const { return 0; }
    const Translation::Storage& NullWindowManager::getTranslationDataStorage() const { return mTranslationDataStorage; }
    Loading::Listener* NullWindowManager::getLoadingScreen() { return &mLoadingListener; }
    bool NullWindowManager::getCursorVisible() { return mCursorVisible; }
    size_t NullWindowManager::countSavedGameRecords() const { return 0; }
    bool NullWindowManager::isSavingAllowed() const { return true; }
    const MWGui::TextColours& NullWindowManager::getTextColours() { return mTextColours; }
    bool NullWindowManager::injectKeyPress(MyGUI::KeyCode, unsigned int, bool) { return false; }
    bool NullWindowManager::injectKeyRelease(MyGUI::KeyCode) { return false; }
    bool NullWindowManager::isWindowVisible() const { return mWindowVisible; }
    void NullWindowManager::windowVisibilityChange(bool visible) { mWindowVisible = visible; }
    void NullWindowManager::windowClosed()
    {
        mWindowVisible = false;
        if (mRequestQuit)
            mRequestQuit();
    }
    MWWorld::Ptr NullWindowManager::getWatchedActor() const { return mEmptyPtr; }
    const std::string& NullWindowManager::getVersionDescription() const { return mVersionDescription; }
    uint32_t NullWindowManager::getCullMask() { return 0xffffffffu; }
    MWGui::WindowBase* NullWindowManager::getActiveControllerWindow() { return nullptr; }
    int NullWindowManager::getControllerMenuHeight() { return 0; }
    bool NullWindowManager::getControllerTooltipVisible() const { return false; }
    bool NullWindowManager::getControllerTooltipEnabled() const { return false; }
    const std::vector<GuiMode>& NullWindowManager::getGuiModeStack() const
    {
        static const std::vector<GuiMode> empty;
        return empty;
    }
    bool NullWindowManager::isWindowVisible(std::string_view) const { return false; }
    std::vector<std::string_view> NullWindowManager::getAllWindowIds() const { return {}; }
    std::vector<std::string_view> NullWindowManager::getAllowedWindowIds(GuiMode) const { return {}; }
}
