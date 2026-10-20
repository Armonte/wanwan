"""Glicko-2 rating system (Glickman, "Example of the Glicko-2 system", 2013).

Pure functions, no I/O. The stats site feeds matches through `update()` in
time order, one game id at a time (see ratings.py).

Rating period choice: every match is its own rating period with a single
game in it. That is what the hub's log gives us naturally (one record per
settled match) and it lets a rating move right after each match instead of
once per day. Consequence: RD only shrinks through play; we do not inflate
RD for idle calendar time (step 6 runs once per match, not once per idle
day). If idle inflation is wanted later, call `idle()` once per idle period
before a player's next match.

Constants follow the paper: tau = 0.5, a new player starts at 1500 / 350 /
0.06, convergence tolerance epsilon = 0.000001, and the Illinois variant of
regula falsi for the volatility step.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

TAU = 0.5
INITIAL_RATING = 1500.0
INITIAL_RD = 350.0
INITIAL_VOL = 0.06
EPSILON = 0.000001
SCALE = 173.7178  # Glicko-1 <-> Glicko-2 scale factor (400 / ln 10)


@dataclass
class Rating:
    rating: float = INITIAL_RATING
    rd: float = INITIAL_RD
    vol: float = INITIAL_VOL


def _g(phi: float) -> float:
    return 1.0 / math.sqrt(1.0 + 3.0 * phi * phi / (math.pi * math.pi))


def _e(mu: float, mu_j: float, phi_j: float) -> float:
    return 1.0 / (1.0 + math.exp(-_g(phi_j) * (mu - mu_j)))


def _new_volatility(phi: float, sigma: float, delta: float, v: float,
                    tau: float) -> float:
    """Step 5 of the paper: solve for sigma' with the Illinois algorithm."""
    a = math.log(sigma * sigma)

    def f(x: float) -> float:
        ex = math.exp(x)
        num = ex * (delta * delta - phi * phi - v - ex)
        den = 2.0 * (phi * phi + v + ex) ** 2
        return num / den - (x - a) / (tau * tau)

    big_a = a
    if delta * delta > phi * phi + v:
        big_b = math.log(delta * delta - phi * phi - v)
    else:
        k = 1
        while f(a - k * tau) < 0:
            k += 1
        big_b = a - k * tau
    f_a = f(big_a)
    f_b = f(big_b)
    while abs(big_b - big_a) > EPSILON:
        big_c = big_a + (big_a - big_b) * f_a / (f_b - f_a)
        f_c = f(big_c)
        if f_c * f_b <= 0:
            big_a, f_a = big_b, f_b
        else:
            f_a /= 2.0
        big_b, f_b = big_c, f_c
    return math.exp(big_a / 2.0)


def update(player: Rating, results: list[tuple[Rating, float]],
           tau: float = TAU) -> Rating:
    """Return the player's rating after one rating period.

    `results` is a list of (opponent rating at the start of the period,
    score) with score 1.0 = win, 0.5 = draw, 0.0 = loss. An empty list is
    the paper's "did not compete" case (RD grows, rating and volatility
    unchanged)."""
    mu = (player.rating - INITIAL_RATING) / SCALE
    phi = player.rd / SCALE
    sigma = player.vol
    if not results:
        return idle(player)

    v_inv = 0.0
    delta_sum = 0.0
    for opp, score in results:
        mu_j = (opp.rating - INITIAL_RATING) / SCALE
        phi_j = opp.rd / SCALE
        g_j = _g(phi_j)
        e_j = _e(mu, mu_j, phi_j)
        v_inv += g_j * g_j * e_j * (1.0 - e_j)
        delta_sum += g_j * (score - e_j)
    v = 1.0 / v_inv
    delta = v * delta_sum

    sigma_new = _new_volatility(phi, sigma, delta, v, tau)
    phi_star = math.sqrt(phi * phi + sigma_new * sigma_new)
    phi_new = 1.0 / math.sqrt(1.0 / (phi_star * phi_star) + 1.0 / v)
    mu_new = mu + phi_new * phi_new * delta_sum
    return Rating(rating=SCALE * mu_new + INITIAL_RATING,
                  rd=SCALE * phi_new,
                  vol=sigma_new)


def idle(player: Rating) -> Rating:
    """One rating period with no games: phi' = sqrt(phi^2 + sigma^2)."""
    phi = player.rd / SCALE
    phi_new = math.sqrt(phi * phi + player.vol * player.vol)
    return Rating(rating=player.rating,
                  rd=min(INITIAL_RD, SCALE * phi_new),
                  vol=player.vol)


def rate_match(a: Rating, b: Rating, score_a: float,
               tau: float = TAU) -> tuple[Rating, Rating]:
    """One match as its own rating period for both players. Both updates
    use the pre-match ratings, as the paper requires."""
    return (update(a, [(b, score_a)], tau),
            update(b, [(a, 1.0 - score_a)], tau))
