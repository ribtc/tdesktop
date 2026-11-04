/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "calls/calls_network.h"

#include "boxes/abstract_box.h"
#include "lang/lang_keys.h"
#include "ui/boxes/confirm_box.h"

namespace Calls {

bool CheckMediaAllowed(
		const MTP::NetworkSnapshot &network,
		MediaTransport transport) {
	if (MediaAllowed(network, transport)) {
		return true;
	}
	Ui::show(Ui::MakeInformBox(network.allowed()
		? tr::lng_call_network_unsupported(tr::now)
		: tr::lng_call_network_blocked(tr::now)));
	return false;
}

} // namespace Calls
