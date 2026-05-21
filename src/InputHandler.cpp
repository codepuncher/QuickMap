#include "PCH.h"

#include "InputHandler.h"

InputHandler* InputHandler::GetSingleton()
{
	static InputHandler instance;
	return &instance;
}

void InputHandler::SetButtons(std::vector<ButtonConfig> a_configs)
{
	_buttons.clear();
	_buttons.reserve(a_configs.size());
	for (auto& cfg : a_configs) {
		ButtonState state;
		static_cast<ButtonConfig&>(state) = std::move(cfg);
		_buttons.push_back(std::move(state));
	}
}

void InputHandler::UpdateShortPressBinding()
{
	auto* controlMap = RE::ControlMap::GetSingleton();
	if (!controlMap) {
		logger::error("ControlMap unavailable — short press will have no effect");
		for (auto& bs : _buttons) {
			bs.shortPressUserEvent = "";
		}
		return;
	}

	for (auto& bs : _buttons) {
		bs.shortPressUserEvent = controlMap->GetUserEventName(bs.keyCode, RE::INPUT_DEVICE::kGamepad);
		if (bs.shortPressUserEvent.empty()) {
			logger::warn("{} has no binding in ControlMap — short press disabled", bs.name);
		} else {
			logger::info("{} short press user event: '{}'", bs.name, bs.shortPressUserEvent);
		}
	}
}

RE::BSEventNotifyControl InputHandler::ProcessEvent(
	const RE::MenuOpenCloseEvent* a_event,
	RE::BSTEventSource<RE::MenuOpenCloseEvent>* /*a_eventSource*/)
{
	if (a_event && a_event->menuName == RE::JournalMenu::MENU_NAME) {
		if (!a_event->opening) {
			// Journal closed — restore the original tab index so the player's next
			// normal Journal open lands on their own last-visited tab, not the one
			// the plugin forced.
			if (_tabRestorePending) {
				RestoreJournalTab();
			}
			UpdateShortPressBinding();
		}
	}
	return RE::BSEventNotifyControl::kContinue;
}

RE::BSEventNotifyControl InputHandler::ProcessEvent(
	RE::InputEvent* const* a_events,
	RE::BSTEventSource<RE::InputEvent*>* /*a_eventSource*/)
{
	if (!a_events) {
		return RE::BSEventNotifyControl::kContinue;
	}

	auto* ui = RE::UI::GetSingleton();

	// Fail-safe: if a tab restore is pending but the Journal is not open (or UI singleton
	// is unavailable), the Journal failed to open or the close event was not delivered —
	// restore sJournalTabIdx now rather than leaving the forced value in place indefinitely.
	// Safe to check here: dispatch queues AddMessage for the next frame, so by the time
	// we receive further input events the Journal must already be open (game paused) or
	// have never opened. The Journal open case is excluded by IsMenuOpen.
	if (_tabRestorePending && (!ui || !ui->IsMenuOpen(RE::JournalMenu::MENU_NAME))) {
		RestoreJournalTab();
	}

	// If any pausing menu is open, pass all input through and clear any captured press so it
	// can't fire a spurious dispatch once the menu closes.
	if (ui && ui->GameIsPaused()) {
		for (auto& bs : _buttons) {
			bs.pressTime.reset();
			bs.triggered = false;
		}
		return RE::BSEventNotifyControl::kContinue;
	}

	bool shouldBlock = false;

	for (auto* event = *a_events; event; event = event->next) {
		const auto* btn = event->AsButtonEvent();
		if (!btn) {
			continue;
		}
		if (btn->GetDevice() != RE::INPUT_DEVICE::kGamepad) {
			continue;
		}
		for (auto& bs : _buttons) {
			if (btn->GetIDCode() == bs.keyCode) {
				if (ProcessButton(btn, bs)) {
					shouldBlock = true;
				}
			}
		}
	}

	// kStop halts the entire frame's event batch for all downstream sinks. With both Start
	// and Back tracked by default, this fires on every press of either managed button.
	// Pressing any other input while a managed button hold is in progress is suppressed from
	// downstream sinks. This is intentional: hold detection requires exclusive ownership of
	// those frames. Selective kStop per event is not feasible with CommonLib's batch API.
	return shouldBlock ? RE::BSEventNotifyControl::kStop : RE::BSEventNotifyControl::kContinue;
}

bool InputHandler::ProcessButton(const RE::ButtonEvent* btn, ButtonState& state)
{
	if (btn->IsDown()) {
		state.pressTime = std::chrono::steady_clock::now();
		state.triggered = false;
		return true;
	}

	if (btn->IsHeld() && state.pressTime) {
		if (!state.triggered && btn->HeldDuration() >= holdDuration) {
			state.triggered = true;
			DispatchLongPress(state);
		}
		return true;
	}

	if (btn->IsUp() && state.pressTime) {
		if (state.triggered) {
			state.triggered = false;
		} else {
			const auto held = std::chrono::duration<float>(
				std::chrono::steady_clock::now() - *state.pressTime)
			                      .count();
			DispatchShortPress(state, held);
		}
		state.pressTime.reset();
		return true;
	}

	return false;
}

void InputHandler::DispatchLongPress(const ButtonState& state)
{
	if (state.action == LongPressAction::kMap) {
		auto* uiQueue = RE::UIMessageQueue::GetSingleton();
		if (!uiQueue) {
			logger::error("{} long press: UIMessageQueue unavailable — action not dispatched", state.name);
			return;
		}
		logger::info("{} long press: opening Map", state.name);
		uiQueue->AddMessage(RE::MapMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
		return;
	}

	auto* userEvents = RE::UserEvents::GetSingleton();
	if (!userEvents) {
		logger::error("{} long press: UserEvents unavailable — action not dispatched", state.name);
		return;
	}

	logger::info("{} long press: opening Journal", state.name);

	JournalTab targetTab = JournalTab::kQuest;
	switch (state.action) {
	case LongPressAction::kSystem:
		targetTab = JournalTab::kSystem;
		break;
	case LongPressAction::kStats:
		targetTab = JournalTab::kStats;
		break;
	default:
		break;  // kQuests: kQuest(0) — game resets sJournalTabIdx to 0 anyway, but we still save/restore
	}

	OpenJournalOnTab(targetTab, state.name);

	if (!DispatchViaMenuOpenHandler(userEvents->journal, state.keyCode, state.name + " long press")) {
		RestoreJournalTab();
		return;
	}

	// Re-write target tab after menuOpenHandler->ProcessButton() resets sJournalTabIdx internally.
	// AddMessage is queued for the next frame so the Journal will read our value.
	if (sJournalTabIdx.get()) {
		*sJournalTabIdx = static_cast<std::uint32_t>(targetTab);
	}
}

bool InputHandler::DispatchViaMenuOpenHandler(
	const RE::BSFixedString& userEvent,
	std::uint32_t            keyCode,
	const std::string&       logContext)
{
	auto* menuControls = RE::MenuControls::GetSingleton();
	if (!menuControls) {
		logger::error("{}: MenuControls unavailable — action not dispatched", logContext);
		return false;
	}
	if (!menuControls->menuOpenHandler) {
		logger::error("{}: menuOpenHandler unavailable — action not dispatched", logContext);
		return false;
	}

	auto deleter = [](RE::ButtonEvent* e) {
		e->~ButtonEvent();
		RE::free(e);
	};
	std::unique_ptr<RE::ButtonEvent, decltype(deleter)> syntheticEvent{
		RE::ButtonEvent::Create(RE::INPUT_DEVICE::kGamepad, userEvent, keyCode, 1.0F, 0.0F),
		deleter
	};
	if (!syntheticEvent) {
		logger::error("{}: failed to allocate synthetic ButtonEvent — action not dispatched", logContext);
		return false;
	}

	if (!menuControls->menuOpenHandler->CanProcess(syntheticEvent.get())) {
		logger::warn("{}: MenuOpenHandler rejected event — action not dispatched", logContext);
		return false;
	}

	menuControls->menuOpenHandler->ProcessButton(syntheticEvent.get());
	return true;
}

void InputHandler::OpenJournalOnTab(JournalTab tab, const std::string& buttonName)
{
	if (!sJournalTabIdx.get()) {
		logger::warn("{} long press: sJournalTabIdx unavailable — Journal will open on default tab", buttonName);
		return;
	}
	if (!_tabRestorePending) {
		_savedTabIdx = static_cast<JournalTab>(*sJournalTabIdx);
	}
	_tabRestorePending = true;
	*sJournalTabIdx = static_cast<std::uint32_t>(tab);
}

void InputHandler::RestoreJournalTab()
{
	if (sJournalTabIdx.get()) {
		*sJournalTabIdx = static_cast<std::uint32_t>(_savedTabIdx);
	}
	_tabRestorePending = false;
}

void InputHandler::DispatchShortPress(const ButtonState& state, float held)
{
	// Best-effort guard against stale pressTime from process suspension (e.g. Alt-Tab).
	// Any legitimate short press has held < holdDuration <= kMaxHoldDuration, so this only
	// fires when pressTime accumulated wall-clock time during suspension while game time
	// was frozen. Known limitation: suspensions shorter than kMaxHoldDuration seconds may
	// still dispatch a spurious short press.
	if (held > kMaxHoldDuration) {
		logger::warn("{} press duration {:.1f}s exceeds sanity limit — discarded", state.name, held);
		return;
	}

	if (state.shortPressUserEvent.empty()) {
		logger::warn("{} short press has no binding — press consumed but no menu opened", state.name);
		return;
	}

	DispatchViaMenuOpenHandler(state.shortPressUserEvent, state.keyCode, state.name + " short press");
}
