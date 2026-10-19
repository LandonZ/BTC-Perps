#include "kalshi-model.hpp"

namespace kalshi {

double NaiveBasisModel::centre_bps() const {
    if (p_.lookback == 0 || window_.empty()) return p_.centre_bps;
    return sum_ / window_.size();
}

Action NaiveBasisModel::on_quote(const Quote& q, int position) {
    // Decide against the centre as it stood before this bar, then add the bar.
    const bool was_ready = ready();
    const double c = centre_bps();

    if (p_.lookback > 0) {
        window_.push_back(q.basis_mid());
        sum_ += window_.back();
        if ((int)window_.size() > p_.lookback) {
            sum_ -= window_.front();
            window_.pop_front();
        }
    }
    if (!was_ready) return Action::Hold;

    if (position == 0) {
        if (q.basis_ask() < c - p_.edge_bps) return Action::Buy;
        if (q.basis_bid() > c + p_.edge_bps) return Action::Sell;
    } else if (position > 0) {
        if (q.basis_bid() >= c - p_.exit_bps) return Action::Sell;
    } else {
        if (q.basis_ask() <= c + p_.exit_bps) return Action::Buy;
    }
    return Action::Hold;
}

}  // namespace kalshi
