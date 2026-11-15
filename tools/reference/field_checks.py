"""Re-implements two of the app's field kernels (same algorithm, same
discretisation constants) and compares them with closed-form results.

1. Rectangular charged plate: app uses a fixed nx*ny grid of point charges
   (24*sqrt(aspect) clamped 12..56). Compare E_z on the axis with the exact
   on-axis formula for a uniformly charged rectangle.
2. Solenoid: app clamps to <= 28 virtual loops, each a 88-segment polygon.
   Compare B_z near the winding with an ideal finite current-sheet solenoid
   computed by fine numerical integration.
"""
import numpy as np

eps0 = 8.8541878128e-12
k_e = 8.9875517923e9
mu0 = 4e-7 * np.pi


# ---------- 1. Rectangular plate ----------
def app_plate_field(Q, W, H, p):
    aspect = W / H
    nx = int(np.clip(round(24 * np.sqrt(aspect)), 12, 56))
    ny = int(np.clip(round(24 / np.sqrt(aspect)), 12, 56))
    dq = Q / (nx * ny)
    xs = (-0.5 + (np.arange(nx) + 0.5) / nx) * W
    ys = (-0.5 + (np.arange(ny) + 0.5) / ny) * H
    X, Y = np.meshgrid(xs, ys)
    r = np.stack([p[0] - X, p[1] - Y, np.full_like(X, p[2])], -1)
    d = np.linalg.norm(r, axis=-1)
    return (k_e * dq * r / d[..., None] ** 3).sum(axis=(0, 1)), nx, ny


def exact_axis_Ez(Q, W, H, z):
    sigma = Q / (W * H)
    a, b = W / 2, H / 2
    return sigma / (np.pi * eps0) * np.arctan(a * b / (z * np.sqrt(a * a + b * b + z * z)))


def exact_plate_field_numeric(Q, W, H, p, n=1200):
    # very fine midpoint rule, used off-axis as the reference
    xs = (-0.5 + (np.arange(n) + 0.5) / n) * W
    ys = (-0.5 + (np.arange(n) + 0.5) / n) * H
    dq = Q / (n * n)
    E = np.zeros(3)
    for y in ys:
        r = np.stack([p[0] - xs, np.full_like(xs, p[1] - y), np.full_like(xs, p[2])], -1)
        d = np.linalg.norm(r, axis=-1)
        E += (k_e * dq * r / d[:, None] ** 3).sum(0)
    return E


Q, W, H = 1e-9, 1.0, 0.6  # app defaults
print("Rectangular plate 1.0 m x 0.6 m, Q = 1 nC (app defaults)")
for z in (0.20, 0.05, 0.02, 0.01, 0.005):
    Eapp, nx, ny = app_plate_field(Q, W, H, (0.0, 0.0, z))
    Eex = exact_axis_Ez(Q, W, H, z)
    print(f"  on axis z={z*100:5.1f} cm : app Ez={Eapp[2]:9.3f} V/m  exact={Eex:9.3f}  err={100*(Eapp[2]-Eex)/Eex:+7.1f} %  (grid {nx}x{ny})")
# off-axis point between two grid nodes, close to the plate
cell = W / nx
for z in (0.01, 0.005):
    p = (cell * 0.0, 0.0, z)  # above a node column (x = 0 lies between nodes for even nx)
    p2 = (cell * 0.5, 0.0, z)  # above a node
    for pt, label in ((p, "between nodes"), (p2, "above a node ")):
        Eapp, _, _ = app_plate_field(Q, W, H, pt)
        Eref = exact_plate_field_numeric(Q, W, H, pt)
        print(f"  {label} z={z*100:4.1f} cm : app Ez={Eapp[2]:9.3f}  ref={Eref[2]:9.3f}  err={100*(Eapp[2]-Eref[2])/Eref[2]:+7.1f} %")


# ---------- 2. Solenoid ----------
def seg_field(p0, p1, I, p):
    # exact finite straight segment (Biot-Savart)
    dl = p1 - p0
    L = np.linalg.norm(dl)
    u = dl / L
    r0 = p - p0
    s = r0 @ u
    rho_v = r0 - s * u
    rho = np.linalg.norm(rho_v)
    if rho < 1e-12:
        return np.zeros(3)
    c1 = s / np.hypot(s, rho)
    c2 = (s - L) / np.hypot(s - L, rho)
    Bmag = 1e-7 * I / rho * (c1 - c2)
    return Bmag * np.cross(u, rho_v / rho)


def loop_field(zc, R, I, p, nseg):
    B = np.zeros(3)
    a = 2 * np.pi * np.arange(nseg + 1) / nseg
    pts = np.stack([R * np.cos(a), R * np.sin(a), np.full_like(a, zc)], -1)
    for i in range(nseg):
        B += seg_field(pts[i], pts[i + 1], I, p)
    return B


def app_solenoid(N, I, R, L, p):
    loops = int(np.clip(N, 1, 28))
    Ieff = I * N / loops
    B = np.zeros(3)
    for i in range(loops):
        zc = -0.5 * L + (i + 0.5) * L / loops
        B += loop_field(zc, R, Ieff, p, 88)
    return B


def sheet_solenoid(N, I, R, L, p, loops=600):
    B = np.zeros(3)
    for i in range(loops):
        zc = -0.5 * L + (i + 0.5) * L / loops
        B += loop_field(zc, R, I * N / loops, p, 360)
    return B


N, I, R, L = 100, 1.0, 0.5, 2.0  # app defaults
print("\nSolenoid N=100, I=1 A, R=0.5 m, L=2 m (app defaults); ideal mu0*n*I =", f"{mu0*N/L*I*1e6:.2f} uT")
for rho in (0.0, 0.40, 0.45, 0.48):
    for z in (0.0, 0.5 * L / 28):  # mid-plane and half a virtual-loop pitch
        p = np.array([rho, 0.0, z])
        Ba = app_solenoid(N, I, R, L, p)
        Bs = sheet_solenoid(N, I, R, L, p)
        print(f"  rho={rho:4.2f} m z={z*100:5.2f} cm : app Bz={Ba[2]*1e6:8.3f} uT  sheet={Bs[2]*1e6:8.3f} uT  err={100*(Ba[2]-Bs[2])/Bs[2]:+6.2f} %")
