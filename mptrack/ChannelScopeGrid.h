/*
 * ChannelScopeGrid.h
 * ------------------
 * Назначение: осциллографы по каналам — сетка маленьких окон волны.
 * Заметки  : Это одно окно, а не по окну на канал: у каждого окна свой таймер
 *            и свой холст, и на двадцати каналах это обошлось бы дороже самой
 *            отрисовки. Здесь один таймер и один холст на всю сетку.
 * Авторы   : OpenMPT Devs
 * Исходники OpenMPT распространяются по лицензии BSD. Подробности в LICENSE.
 */

#pragma once

#include "openmpt/all/BuildSettings.hpp"
#include "../soundlib/ChannelScopes.h"
#include "../misc/mptClock.h"
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

OPENMPT_NAMESPACE_BEGIN

class ChannelScopeGrid : public CWnd
{
public:
	bool Create(CWnd *parent, UINT id);
	// Вкладка говорит, где сетке стоять; включение капчи в микшере она берёт
	// на себя сама, по тому, видно её или нет.
	void Place(const CRect &rect, bool visible);
	// Во весь экран и обратно: окно уходит из панели на отдельный слой поверх
	// всего, а потом возвращается на своё место.
	void ToggleFullScreen();
	bool IsFullScreen() const { return m_fullScreen || m_owner != nullptr; }
	~ChannelScopeGrid();

protected:
	// Сколько отсчётов видно в одном окошке. Четверть кольца: больше брать
	// незачем, картинка и так шире экрана.
	static constexpr uint32 Span = 1024;

	std::vector<float> m_samples;
	std::vector<CPoint> m_points;
	CDC m_offScreenDC;
	CBitmap m_offScreenBitmap;
	CSize m_offScreenSize{0, 0};
	// Усиление на канал подбирается само и отпускается медленно: иначе тихая
	// нота после громкой прыгала бы на весь экран.
	std::array<float, ChannelScopes::Channels> m_gain{};
	bool m_running = false;
	// Кадры шлёт отдельный поток: таймер окна не умеет чаще шестидесяти раз в
	// секунду и округляет сон до пятнадцати миллисекунд.
	std::unique_ptr<Util::MultimediaClock> m_clock;
	std::thread m_ticker;
	std::atomic<bool> m_ticking{false};
	std::atomic<bool> m_pending{false};
	int m_interval = 16;
	// Полный экран — отдельное окно-копия поверх всего. Сама сетка остаётся в
	// панели: раньше её вынимали из окна и вставляли обратно, и обратно она
	// вставала на всё окно трека.
	bool m_fullScreen = false;
	ChannelScopeGrid *m_popup = nullptr;   // у сетки в панели — её копия на весь экран
	ChannelScopeGrid *m_owner = nullptr;   // у копии — сетка, которой она принадлежит
	bool CreatePopup(ChannelScopeGrid &owner, const CRect &screen);
	void HideScopes();
	void PostNcDestroy() override;
	afx_msg LRESULT OnClosePopup(WPARAM, LPARAM);
	CWnd *m_parent = nullptr;
	CRect m_placed{0, 0, 0, 0};

	void StartTicker();
	void StopTicker();

	void Draw(CDC &dc, const CRect &client);
	// Ищет точку, с которой волна стоит на месте, а не ездит по экрану.
	static uint32 Trigger(const float *samples, uint32 count, float peak);

	afx_msg LRESULT OnFrame(WPARAM, LPARAM);
	afx_msg void OnPaint();
	afx_msg void OnDestroy();
	afx_msg void OnRButtonUp(UINT flags, CPoint point);
	afx_msg void OnLButtonDblClk(UINT flags, CPoint point);
	afx_msg void OnKeyDown(UINT key, UINT repeat, UINT flags);
	afx_msg BOOL OnEraseBkgnd(CDC *) { return TRUE; }
	DECLARE_MESSAGE_MAP()
};

OPENMPT_NAMESPACE_END
