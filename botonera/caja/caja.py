"""Caja de la botonera, versión 5 "tablet" con 5 perillas: placa finita apenas inclinada, tapa biselada que cierra con imanes.

Genera en esta carpeta:
    base.stl   pieza 1, se imprime tal como sale
    tapa.stl   pieza 2, se imprime boca abajo (la cara linda contra la cama)
y para el visor (no se imprimen): base.json, tapa_armada.json, ref_*.json

Uso: python3 caja.py   (necesita: pip install manifold3d numpy)
Medidas en milímetros. Las de las piezas compradas están arriba, para corregirlas con las piezas en mano.
"""
import json
import math
import os
import struct

import numpy as np
from manifold3d import CrossSection, Manifold

AQUI = os.path.dirname(os.path.abspath(__file__))

# ---- Forma ----
ANCHO = 170            # X
FONDO = 118            # Y, de adelante (vos) hacia atrás
ALTO_ADELANTE = 26
ALTO_ATRAS = 34
RADIO = 14             # esquinas vistas desde arriba
BISEL = 2.0            # bisel del borde de la tapa
PARED = 2.4
PISO = 2.4
TAPA = 3.5             # espesor de la tapa (perpendicular a ella)

# ---- Pantalla CYD (medida con calibre el 20/10: placa 86 x 50, pantalla 69,5 x 50, 7,2 de espesor total) ----
PLACA = (86, 50)
PANEL = (70, 50.5)
PANEL_ESPESOR = 5.6             # del frente de la placa amarilla a la cara del vidrio (7,2 - 1,6 de placa)
REBAJE = 1.0
VENTANA = (60, 45)
# Agujeros: sacados de las fotos con escala, ±1 mm. Si los tornillos no coinciden, va con cinta doble faz gruesa.
AGUJEROS_PLACA = [(-39.25, -21.25), (39.25, -21.25), (-39.25, 21.25), (39.25, 21.25)]
CENTRO_PANTALLA_V = 64          # desde el borde delantero de la tapa, medido sobre la pendiente

# ---- Perillas ----
PERILLAS_X = [37, 61, 85, 109, 133]   # 4 de luces + la de color (la de la derecha), centradas con la pantalla
PERILLAS_V = 17
AGUJERO_PERILLA = 7.4
PLATITO = 11.0                  # radio del rebaje alrededor de cada perilla
PLATITO_HONDO = 1.0

# ---- Imanes de neodimio 6 x 3 mm ----
IMAN = (6.2, 3.2)               # diámetro y alto del hueco
TETON = 3.0                     # cuánto baja el soporte del imán desde la tapa

# ---- Costado derecho: USB-C del cargador e interruptor ----
USB_C = dict(y=22.5, z=PISO + 5.0, ancho=9.6, alto=3.8)
INTERRUPTOR = dict(y=66, z=PISO + 9.0, ancho=9.0, alto=4.4)
# ---- Expansores con cables dupont enchufados, en la franja de atrás ----
EXPANSOR = (50, 20)
ALTO_DUPONT = 23.0               # placa + pines + conector dupont + curva del cable
MARGEN = 3.0
PORTAPILA = (21, 77, 19)        # acostado a lo largo, en la franja izquierda
PILA_X = PARED + 12               # dónde empieza el portapila (deja lugar al imán de la esquina)
PILA_Y = FONDO - PARED - 2 - PORTAPILA[1]

SEG = 72
ANG = math.atan2(ALTO_ATRAS - ALTO_ADELANTE, FONDO)
TAPA_V = TAPA / math.cos(ANG)             # espesor de la tapa medido en vertical
CORTE = ALTO_ADELANTE - TAPA_V            # altura del corte base/tapa en el borde delantero
LARGO_TAPA = FONDO / math.cos(ANG)


def contorno(inset=0.0):
    r = RADIO - inset
    return CrossSection.square((ANCHO - 2 * RADIO, FONDO - 2 * RADIO)).translate((RADIO, RADIO)).offset(r, circular_segments=SEG)


def prisma(inset=0.0, z0=0.0, alto=ALTO_ATRAS + 20):
    return contorno(inset).extrude(alto).translate((0, 0, z0))


def plano(m, altura_adelante, debajo=True):
    """Corta con un plano paralelo a la tapa que pasa a esa altura en el borde delantero."""
    n = np.array([0.0, math.tan(ANG), -1.0])
    n = n / np.linalg.norm(n)
    d = float(np.dot(n, (0, 0, altura_adelante)))
    return m.trim_by_plane(tuple(n), d) if debajo else m.trim_by_plane(tuple(-n), -d)


def a_tapa(m):
    """Lleva una pieza dibujada en el sistema de la tapa (Z=0 su cara de abajo) a su lugar en la caja."""
    return m.rotate((math.degrees(ANG), 0, 0)).translate((0, 0, CORTE))


def de_tapa(m):
    return m.translate((0, 0, -CORTE)).rotate((-math.degrees(ANG), 0, 0))


def cilindro(r, h, x, y, z=0.0):
    return Manifold.cylinder(h, r, circular_segments=SEG).translate((x, y, z))


def caja_centrada(sx, sy, sz, cx, cy, z0):
    return Manifold.cube((sx, sy, sz)).translate((cx - sx / 2, cy - sy / 2, z0))


def ranura_derecha(y, z, ancho, alto):
    """Ranura que atraviesa la pared derecha."""
    largo = PARED + 3
    return ranura(0, 0, 0, ancho, alto, largo).rotate((0, 0, 90)).translate((ANCHO + 1, y, z))


def ranura(x, y, z, ancho, alto, largo):
    r = alto / 2
    perfil = CrossSection.square((ancho - alto, alto)).translate((-(ancho - alto) / 2, -r)).offset(r, circular_segments=SEG)
    return perfil.extrude(largo).rotate((-90, 0, 0)).translate((x, y, z))


# Cuña completa con el bisel arriba: envolvente de la cuña entera y de la misma cuña achicada por el bisel
def cuna():
    alta = plano(prisma(), ALTO_ADELANTE - BISEL / math.cos(ANG))
    achicada = plano(prisma(BISEL), ALTO_ADELANTE)
    return Manifold.batch_hull([alta, achicada])


POSTES = [(PARED + 5.5, PARED + 5.5), (ANCHO - PARED - 5.5, PARED + 5.5),
          (PARED + 5.5, FONDO - PARED - 5.5), (ANCHO - PARED - 5.5, FONDO - PARED - 5.5)]


def base():
    cuerpo = plano(cuna(), CORTE)
    cuerpo -= prisma(PARED, PISO)
    for x, y in POSTES:
        cuerpo += plano(cilindro(5, ALTO_ATRAS + 10, x, y), CORTE - TETON / math.cos(ANG))
        # hueco del imán, perpendicular a la tapa
        hueco = a_tapa(Manifold.cylinder(IMAN[1] + 1, IMAN[0] / 2, circular_segments=SEG).translate((x, y / math.cos(ANG), -TETON - IMAN[1])))
        cuerpo -= hueco
    # patitas de goma abajo
    for x, y in ((16, 16), (ANCHO - 16, 16), (16, FONDO - 16), (ANCHO - 16, FONDO - 16)):
        cuerpo -= cilindro(5.2, 1.0, x, y, -0.01)
    # guías del portapila
    for x in (PILA_X - 2, PILA_X + PORTAPILA[0]):
        cuerpo += Manifold.cube((2, 50, 4)).translate((x, PILA_Y + 14, PISO - 0.1))
    # cunita del cargador para que el USB-C quede a la altura del agujero
    cx = ANCHO - PARED - 15
    cuerpo += caja_centrada(30, 19, 1.8, cx, USB_C["y"], PISO - 0.1)
    for dy in (-10.5, 9.5):
        cuerpo += Manifold.cube((30, 1.0, 4)).translate((cx - 15, USB_C["y"] + dy, PISO - 0.1))
    cuerpo -= ranura_derecha(USB_C["y"], USB_C["z"], USB_C["ancho"], USB_C["alto"])
    cuerpo -= ranura_derecha(INTERRUPTOR["y"], INTERRUPTOR["z"], INTERRUPTOR["ancho"], INTERRUPTOR["alto"])
    return cuerpo


def tapa():
    t = plano(cuna(), CORTE, debajo=False)
    cx, cv = ANCHO / 2, CENTRO_PANTALLA_V
    # ventana con bisel a 45° hacia afuera
    ventana = Manifold.batch_hull([
        caja_centrada(VENTANA[0], VENTANA[1], TAPA - 1.2, cx, cv, -1),
        caja_centrada(VENTANA[0] + 3, VENTANA[1] + 3, 1.0, cx, cv, TAPA + 0.29)])
    rebaje = caja_centrada(PANEL[0] + 0.6, PANEL[1] + 0.6, REBAJE + 0.01, cx, cv, -0.01)
    agujeros = ventana + rebaje
    for x in PERILLAS_X:
        agujeros += cilindro(AGUJERO_PERILLA / 2, TAPA + 4, x, PERILLAS_V, -2)
        agujeros += cilindro(PLATITO, 3, x, PERILLAS_V, TAPA - PLATITO_HONDO)
    t -= a_tapa(agujeros)

    agregados = Manifold()
    # pollera interna que centra la tapa sobre la base
    anillo = (contorno(PARED + 0.5) - contorno(PARED + 1.7)).extrude(ALTO_ATRAS + 10)
    t += plano(plano(anillo, CORTE + 0.01), CORTE - 2.5 / math.cos(ANG), debajo=False)
    # columnitas de la pantalla
    alto = PANEL_ESPESOR - REBAJE
    for dx, dv in AGUJEROS_PLACA:
        agregados += cilindro(3.5, alto + 0.01, cx + dx, cv + dv, -alto)   # punta ancha: sirve para tornillo o para cinta
    # soportes de los imanes
    for x, y in POSTES:
        agregados += cilindro(5, TETON + 0.01, x, y / math.cos(ANG), -TETON)
    t += a_tapa(agregados)

    huecos = Manifold()
    for dx, dv in AGUJEROS_PLACA:
        huecos += cilindro(1.35, alto + 1, cx + dx, cv + dv, -alto - 1)
    for x, y in POSTES:
        huecos += cilindro(IMAN[0] / 2, IMAN[1] + 0.01, x, y / math.cos(ANG), -TETON - 0.01)
    return t - a_tapa(huecos)


def referencias():
    cx, cv = ANCHO / 2, CENTRO_PANTALLA_V
    pantalla = (caja_centrada(PANEL[0], PANEL[1], PANEL_ESPESOR, cx, cv, REBAJE - PANEL_ESPESOR)
                + caja_centrada(PLACA[0], PLACA[1], 1.6, cx, cv, REBAJE - PANEL_ESPESOR - 1.6))
    vidrio = caja_centrada(VENTANA[0] - 1, VENTANA[1] - 1, 0.2, cx, cv, REBAJE - 0.2)
    perillas = Manifold()
    for x in PERILLAS_X:
        perillas += Manifold.cylinder(15, 8.5, 8.0, circular_segments=SEG).translate((x, PERILLAS_V, TAPA - PLATITO_HONDO + 1.5))
        perillas += caja_centrada(19, 26, 1.6, x, PERILLAS_V + 5, -9)
    pila = Manifold.cube(PORTAPILA).translate((PILA_X, PILA_Y, PISO))
    # plaquitas: cargador y elevador a la derecha, los dos expansores (con sus dupont) en la franja de atrás
    modulos = (caja_centrada(28, 17, 4, ANCHO - PARED - 15, USB_C["y"], PISO + 1.8)
               + caja_centrada(37, 17, 6, ANCHO - PARED - 20, 45, PISO))
    for x in EXPANSORES_X:
        modulos += caja_centrada(EXPANSOR[0], EXPANSOR[1], ALTO_DUPONT, x, EXPANSOR_Y, PISO)
    return a_tapa(pantalla), a_tapa(vidrio), a_tapa(perillas), pila, modulos


EXPANSORES_X = [70, 124]
EXPANSOR_Y = FONDO - PARED - 3.5 - EXPANSOR[1] / 2   # lejos de la pollera de la tapa


def chequear(pantalla, perillas, pila, modulos, b, t):
    """Avisa si algo de adentro choca con la base o la tapa (a los dupont les suma el margen)."""
    con_margen = Manifold()
    for x in EXPANSORES_X:
        con_margen += caja_centrada(EXPANSOR[0], EXPANSOR[1], ALTO_DUPONT + MARGEN, x, EXPANSOR_Y, PISO + 0.01)
    for nombre, m in (("pantalla", pantalla), ("perillas", perillas), ("pila", pila), ("modulos", modulos),
                      ("dupont + margen", con_margen)):
        for pieza, otra in (("base", b), ("tapa", t)):
            v = (m ^ otra).volume()
            if v > 1:
                print(f"  ojo: {nombre} se mete {v:.0f} mm3 en la {pieza}")
    print("  luz entre pila y tapa:", round(CORTE + (PILA_Y) * math.tan(ANG) - (PISO + PORTAPILA[2]), 1), "mm")


def triangulos(m):
    malla = m.to_mesh()
    v = np.asarray(malla.vert_properties)[:, :3]
    return v[np.asarray(malla.tri_verts)]


def guardar_stl(m, nombre):
    tri = triangulos(m)
    with open(os.path.join(AQUI, nombre), "wb") as f:
        f.write(b"botonera".ljust(80, b" "))
        f.write(struct.pack("<I", len(tri)))
        for p0, p1, p2 in tri:
            n = np.cross(p1 - p0, p2 - p0)
            ln = np.linalg.norm(n)
            f.write(struct.pack("<12fH", *(n / ln if ln else n), *p0, *p1, *p2, 0))


def guardar_json(m, nombre):
    with open(os.path.join(AQUI, nombre), "w") as f:
        json.dump([round(float(x), 2) for x in triangulos(m).ravel()], f, separators=(",", ":"))


if __name__ == "__main__":
    b, t = base(), tapa()
    pantalla, vidrio, perillas, pila, modulos = referencias()
    chequear(pantalla, perillas, pila, modulos, b, t)
    guardar_stl(b, "base.stl")
    guardar_stl(de_tapa(t).mirror((0, 0, 1)), "tapa.stl")   # boca abajo para imprimir
    for m, n in ((b, "base"), (t, "tapa_armada"), (pantalla, "ref_pantalla"), (vidrio, "ref_vidrio"),
                 (perillas, "ref_perillas"), (pila, "ref_pila"), (modulos, "ref_modulos")):
        guardar_json(m, n + ".json")
    print(f"base {b.volume() / 1000:.0f} cm3, tapa {t.volume() / 1000:.0f} cm3, inclinación {math.degrees(ANG):.1f}°, "
          f"géneros {b.genus()} / {t.genus()}")
