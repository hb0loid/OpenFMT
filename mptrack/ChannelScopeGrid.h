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
#include <vector>

OPENMPT_NAMESPACE_BEGIN

class ChannelScopeGrid : public CWnd
{
public:
	bool Create(CWnd *parent, UINT id);
	// Вкладка говорит, где сетке стоять; включение капчи в микшере она берёт
	// на себя сама, по тому, видно её или нет.
	void Place(const CRect &rect, bool visible);
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

	void Draw(CDC &dc, const CRect &client);
	// Ищет точку, с которой волна стоит на месте, а не ездит по экрану.
	static uint32 Trigger(const float *samples, uint32 count, float peak);

	afx_msg void OnPaint();
	afx_msg void OnTimer(UINT_PTR id);
	afx_msg void OnDestroy();
	afx_msg BOOL OnEraseBkgnd(CDC *) { return TRUE; }
	DECLARE_MESSAGE_MAP()
};

OPENMPT_NAMESPACE_END
