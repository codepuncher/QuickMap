#pragma once

class InputHandler :
	public RE::BSTEventSink<RE::InputEvent*>,
	public RE::BSTEventSink<RE::MenuOpenCloseEvent>
{
public:
	static InputHandler* GetSingleton();

	InputHandler(const InputHandler&) = delete;
	InputHandler(InputHandler&&) = delete;
	InputHandler& operator=(const InputHandler&) = delete;
	InputHandler& operator=(InputHandler&&) = delete;

	RE::BSEventNotifyControl ProcessEvent(
		RE::InputEvent* const*               a_events,
		RE::BSTEventSource<RE::InputEvent*>* a_source) override;

	RE::BSEventNotifyControl ProcessEvent(
		const RE::MenuOpenCloseEvent*               a_event,
		RE::BSTEventSource<RE::MenuOpenCloseEvent>* a_source) override;

	static constexpr float kDefaultHoldDuration{ 0.5F };

	void SetHoldDuration(float a_duration) noexcept { holdDuration = a_duration; }

	// Queries ControlMap for the user event bound to Start and caches it in shortPressUserEvent.
	// Call at kInputLoaded, kPostLoadGame, and kNewGame.
	void UpdateShortPressUserEvent();

	~InputHandler() override = default;

private:
	InputHandler() = default;

	bool ProcessStartButton(const RE::ButtonEvent* btn);
	void DispatchShortPress(float held) const;

	float                                                holdDuration{ kDefaultHoldDuration };
	RE::BSFixedString                                    shortPressUserEvent;
	std::optional<std::chrono::steady_clock::time_point> _pressTime;
	bool                                                 _mapTriggered{ false };
};
