"""Independent reference for the centre-fed dipole input impedance.

Hallen integral equation, reduced kernel, pulse basis, point matching
(N centres + 1 end point), delta-gap source.  Used only to check the
app's own validation-bench reference value and pass threshold.
"""
import numpy as np

c0 = 299792458.0
eta = 376.730313668


def hallen(L_lambda, a_lambda, N, f=100e6):
    lam = c0 / f
    k = 2 * np.pi / lam
    h = 0.5 * L_lambda * lam
    a = a_lambda * lam
    dz = 2 * h / N
    zc = -h + dz * (np.arange(N) + 0.5)
    zm = np.append(zc, h)  # N centres + wire end
    gx, gw = np.polynomial.legendre.leggauss(16)
    A = np.zeros((N + 1, N + 1), complex)
    for m, z in enumerate(zm):
        for n, zn in enumerate(zc):
            zp = zn + 0.5 * dz * gx
            R = np.sqrt((z - zp) ** 2 + a ** 2)
            A[m, n] = np.sum(gw * np.exp(-1j * k * R) / (4 * np.pi * R)) * 0.5 * dz
        A[m, N] = 1j / eta * np.cos(k * z)  # unknown constant C moved to LHS
    V = 1.0
    b = -1j / eta * (V / 2) * np.sin(k * np.abs(zm))
    x = np.linalg.solve(A, b)
    I = x[:N]
    I0 = np.interp(0.0, zc, I.real) + 1j * np.interp(0.0, zc, I.imag)
    return V / I0


if __name__ == "__main__":
    for N in (41, 81, 121, 161):
        print(f"L=0.5 lambda, a=lambda/1000, N={N:4d}: Zin = {hallen(0.5, 1e-3, N):.2f} ohm")
    for L in (0.47, 0.48, 0.49):
        print(f"L={L} lambda, a=lambda/1000, N=121: Zin = {hallen(L, 1e-3, 121):.2f} ohm")
