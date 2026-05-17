#include "PCH.h"

#include "InputHandler.h"

namespace logger = SKSE::log;

InputHandler* InputHandler::GetSingleton()
{
	static InputHandler instance;
	return &instance;
}

void InputHandler::UpdateShortPressUserEvent()
{
	auto* controlMap = RE::ControlMap::GetSingleton();
	if (!controlMap) {
		logger::error("ControlMap unavailable — short press will have no effect");
		shortPressUserEvent = "";
		return;
	}

	constexpr auto kStart = static_cast<std::uint32_t>(RE::BSWin32GamepadDevice::Key::kStart);
	shortPressUserEvent = controlMap->GetUserEventName(kStart, RE::INPUT_DEVICE::kGamepad);

	if (shortPressUserEvent.empty()) {
		logger::warn("Start has no binding in kGameplay context — short press disabled");
	} else {
		logger::info("Start short press user event: '{}'", shortPressUserEvent);
	}
}

RE::BSEventNotifyControl InputHandler::ProcessEvent(
	const RE::MenuOpenCloseEvent* a_event,
	RE::BSTEventSource<RE::MenuOpenCloseEvent>* /*a_eventSource*/)
{
	if (a_event && !a_event->opening && a_event->menuName == RE::JournalMenu::MENU_NAME) {
		UpdateShortPressUserEvent();
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

	// If any pausing menu is open (Journal, Map, Inventory, Console, etc.), pass all input
	// through so the active menu can handle Start normally. Also clears any captured press
	// so _pressTime can't fire a spurious dispatch once the menu closes.
	auto* ui = RE::UI::GetSingleton();
	if (ui && ui->GameIsPaused()) {
		_pressTime.reset();
		_mapTriggered = false;
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
		if (btn->GetIDCode() != static_cast<std::uint32_t>(RE::BSWin32GamepadDevice::Key::kStart)) {
			continue;
		}
		if (ProcessStartButton(btn)) {
			shouldBlock = true;
		}
	}

	return shouldBlock ? RE::BSEventNotifyControl::kStop : RE::BSEventNotifyControl::kContinue;
}

bool InputHandler::ProcessStartButton(const RE::ButtonEvent* btn)
{
	if (btn->IsDown()) {
		_pressTime = std::chrono::steady_clock::now();
		_mapTriggered = false;
		return true;
	}

	if (btn->IsHeld() && _pressTime) {
		if (!_mapTriggered && btn->HeldDuration() >= holdDuration) {
			_mapTriggered = true;
			auto* uiQueue = RE::UIMessageQueue::GetSingleton();
			if (!uiQueue) {
				logger::error("UIMessageQueue unavailable — hold threshold reached but map not opened");
			} else {
				uiQueue->AddMessage(RE::MapMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
			}
		}
		return true;
	}

	if (btn->IsUp() && _pressTime) {
		const auto held = std::chrono::duration<float>(
			std::chrono::steady_clock::now() - *_pressTime)
		                      .count();
		_pressTime.reset();

		if (_mapTriggered) {
			_mapTriggered = false;
		} else {
			DispatchShortPress(held);
		}
		return true;
	}

	return false;
}

void InputHandler::DispatchShortPress(float held) const
{
	if (held > holdDuration + 5.0F) {
		logger::warn("Start press duration {:.1f}s exceeds sanity limit — discarded", held);
		return;
	}

	if (shortPressUserEvent.empty()) {
		logger::warn("Start short press has no binding — press consumed but no menu opened");
		return;
	}

	auto* menuControls = RE::MenuControls::GetSingleton();
	if (!menuControls || !menuControls->menuOpenHandler) {
		logger::error("MenuControls or menuOpenHandler unavailable — short press consumed but no menu opened");
		return;
	}

	constexpr auto kStart = static_cast<std::uint32_t>(RE::BSWin32GamepadDevice::Key::kStart);
	auto           deleter = [](RE::ButtonEvent* e) {
		e->~ButtonEvent();
		RE::free(e);
	};
	std::unique_ptr<RE::ButtonEvent, decltype(deleter)> syntheticEvent{
		RE::ButtonEvent::Create(
			RE::INPUT_DEVICE::kGamepad,
			shortPressUserEvent,
			kStart,
			1.0F,  // value=1.0 → IsPressed()=true, IsDown()=true
			0.0F   // heldDownSecs=0.0 → IsDown()=true
			),
		deleter
	};
	if (!syntheticEvent) {
		logger::error("Failed to allocate synthetic ButtonEvent — short press consumed but no menu opened");
		return;
	}

	if (!menuControls->menuOpenHandler->CanProcess(syntheticEvent.get())) {
		logger::warn("MenuOpenHandler rejected short press — press consumed but no menu opened");
		return;
	}

	menuControls->menuOpenHandler->ProcessButton(syntheticEvent.get());
}
