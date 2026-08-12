#ifndef OPENMW_MWGUI_NULLWINDOWMANAGER_HPP
#define OPENMW_MWGUI_NULLWINDOWMANAGER_HPP

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <components/loadinglistener/loadinglistener.hpp>
#include <components/translation/translation.hpp>

#include "textcolours.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwworld/ptr.hpp"

namespace MWGui
{
    /// Minimal presentation service used by the experimental Vulkan game path.
    /// It intentionally owns no MyGUI or OSG state. Unsupported presentation
    /// requests are no-ops until their renderer-neutral replacement is ready.
    class NullWindowManager final : public MWBase::WindowManager
    {
    public:
        explicit NullWindowManager(std::string versionDescription, std::function<void()> requestQuit = {});

        void playVideo(std::string_view, bool, bool) override;
        void setNewGame(bool) override;
        void pushGuiMode(GuiMode, const MWWorld::Ptr&) override;
        void pushGuiMode(GuiMode) override;
        void popGuiMode(bool) override;
        void removeGuiMode(GuiMode) override;
        void goToJail(int) override;
        void updatePlayer() override;
        GuiMode getMode() const override;
        bool containsMode(GuiMode) const override;
        bool isGuiMode() const override;
        bool isConsoleMode() const override;
        bool isPostProcessorHudVisible() const override;
        bool isSettingsWindowVisible() const override;
        bool isInteractiveMessageBoxActive() const override;
        void toggleVisible(GuiWindow) override;
        void forceHide(GuiWindow) override;
        void unsetForceHide(GuiWindow) override;
        void disallowAll() override;
        void allow(GuiWindow) override;
        bool isAllowed(GuiWindow) const override;
        MWGui::InventoryWindow* getInventoryWindow() override;
        MWGui::CountDialog* getCountDialog() override;
        MWGui::ConfirmationDialog* getConfirmationDialog() override;
        MWGui::TradeWindow* getTradeWindow() override;
        MWGui::HUD* getHud() override;
        MWGui::PostProcessorHud* getPostProcessorHud() override;
        std::vector<MWGui::WindowBase*> getGuiModeWindows(GuiMode) override;
        void useItem(const MWWorld::Ptr&, bool) override;
        void updateSpellWindow() override;
        void setConsoleSelectedObject(const MWWorld::Ptr&) override;
        MWWorld::Ptr getConsoleSelectedObject() const override;
        void setConsoleMode(std::string_view) override;
        const std::string& getConsoleMode() override;
        void printToConsole(const std::string&, std::string_view) override;
        void setDrowningTimeLeft(float, float) override;
        void changeCell(const MWWorld::CellStore*) override;
        void setFocusObject(const MWWorld::Ptr&) override;
        void setFocusObjectScreenCoords(float, float) override;
        void setCursorVisible(bool) override;
        void setCursorActive(bool) override;
        void getMousePosition(int&, int&) override;
        void getMousePosition(float&, float&) override;
        void setDragDrop(bool) override;
        bool getWorldMouseOver() override;
        float getScalingFactor() const override;
        bool toggleFogOfWar() override;
        bool toggleFullHelp() override;
        bool getFullHelp() const override;
        void setDrowningBarVisibility(bool) override;
        void setHMSVisibility(bool) override;
        void setMinimapVisibility(bool) override;
        void setWeaponVisibility(bool) override;
        void setSpellVisibility(bool) override;
        void setSneakVisibility(bool) override;
        void activateQuickKey(int) override;
        void updateActivatedQuickKey() override;
        const ESM::RefId& getSelectedSpell() override;
        void setSelectedSpell(const ESM::RefId&, int) override;
        void setSelectedEnchantItem(const MWWorld::Ptr&) override;
        const MWWorld::Ptr& getSelectedEnchantItem() const override;
        void setSelectedWeapon(const MWWorld::Ptr&) override;
        const MWWorld::Ptr& getSelectedWeapon() const override;
        void unsetSelectedSpell() override;
        void unsetSelectedWeapon() override;
        void showCrosshair(bool) override;
        bool setHudVisibility(bool) override;
        bool isHudVisible() const override;
        void disallowMouse() override;
        void allowMouse() override;
        void notifyInputActionBound() override;
        void addVisitedLocation(const std::string&, int, int) override;
        void removeDialog(std::unique_ptr<MWGui::Layout>&&) override;
        void exitCurrentGuiMode() override;
        void messageBox(std::string_view, MWGui::ShowInDialogueMode) override;
        void scheduleMessageBox(std::string, MWGui::ShowInDialogueMode) override;
        void staticMessageBox(std::string_view) override;
        void removeStaticMessageBox() override;
        void interactiveMessageBox(std::string_view, const std::vector<std::string>&, bool, int) override;
        int readPressedButton() override;
        void updateConsoleObjectPtr(const MWWorld::Ptr&, const MWWorld::Ptr&) override;
        std::string_view getGameSettingString(std::string_view, std::string_view defaultValue) override;
        void processChangedSettings(const std::set<std::pair<std::string, std::string>>&) override;
        void executeInConsole(const std::filesystem::path&) override;
        void enableRest() override;
        bool getRestEnabled() override;
        bool getJournalAllowed() override;
        bool getPlayerSleeping() override;
        void wakeUpPlayer() override;
        void showSoulgemDialog(MWWorld::Ptr) override;
        void changePointer(const std::string&) override;
        void setEnemy(const MWWorld::Ptr&) override;
        std::size_t getMessagesCount() const override;
        const Translation::Storage& getTranslationDataStorage() const override;
        void setKeyFocusWidget(MyGUI::Widget*) override;
        Loading::Listener* getLoadingScreen() override;
        bool getCursorVisible() override;
        void clear() override;
        void write(ESM::ESMWriter&, Loading::Listener&) override;
        void readRecord(ESM::ESMReader&, uint32_t) override;
        size_t countSavedGameRecords() const override;
        bool isSavingAllowed() const override;
        void exitCurrentModal() override;
        void addCurrentModal(MWGui::WindowModal*) override;
        void removeCurrentModal(MWGui::WindowModal*) override;
        void pinWindow(GuiWindow) override;
        void toggleMaximized(MWGui::Layout*) override;
        void fadeScreenIn(float, bool, float) override;
        void fadeScreenOut(float, bool, float) override;
        void fadeScreenTo(int, float, bool, float) override;
        void setBlindness(int) override;
        void activateHitOverlay(bool) override;
        void setWerewolfOverlay(bool) override;
        void toggleConsole() override;
        void toggleDebugWindow() override;
        void togglePostProcessorHud() override;
        void toggleSettingsWindow() override;
        void cycleSpell(bool) override;
        void cycleWeapon(bool) override;
        void playSound(const ESM::RefId&, float, float) override;
        void addCell(MWWorld::CellStore*) override;
        void removeCell(MWWorld::CellStore*) override;
        void writeFog(MWWorld::CellStore*) override;
        const MWGui::TextColours& getTextColours() override;
        bool injectKeyPress(MyGUI::KeyCode, unsigned int, bool) override;
        bool injectKeyRelease(MyGUI::KeyCode) override;
        void windowVisibilityChange(bool) override;
        void windowResized(int, int) override;
        void windowClosed() override;
        bool isWindowVisible() const override;
        void watchActor(const MWWorld::Ptr&) override;
        MWWorld::Ptr getWatchedActor() const override;
        const std::string& getVersionDescription() const override;
        void onDeleteCustomData(const MWWorld::Ptr&) override;
        void forceLootMode(const MWWorld::Ptr&) override;
        void asyncPrepareSaveMap() override;
        void setCullMask(uint32_t) override;
        uint32_t getCullMask() override;
        void inventoryUpdated(const MWWorld::Ptr&) const override;
        MWGui::WindowBase* getActiveControllerWindow() override;
        int getControllerMenuHeight() override;
        void cycleActiveControllerWindow(bool) override;
        void setActiveControllerWindow(GuiMode, size_t) override;
        bool getControllerTooltipVisible() const override;
        void setControllerTooltipVisible(bool) override;
        bool getControllerTooltipEnabled() const override;
        void setControllerTooltipEnabled(bool) override;
        void restoreControllerTooltips() override;
        void updateControllerButtonsOverlay() override;
        const std::vector<GuiMode>& getGuiModeStack() const override;
        void setDisabledByLua(std::string_view, bool) override;
        bool isWindowVisible(std::string_view) const override;
        std::vector<std::string_view> getAllWindowIds() const override;
        std::vector<std::string_view> getAllowedWindowIds(GuiMode) const override;

    private:
        std::string mVersionDescription;
        MWWorld::Ptr mEmptyPtr;
        Translation::Storage mTranslationDataStorage;
        Loading::Listener mLoadingListener;
        MWGui::TextColours mTextColours;
        bool mCursorVisible = true;
        bool mHudVisible = true;
        bool mWindowVisible = true;
        std::function<void()> mRequestQuit;
};
}

#endif
