# ---- shared by the map and the model import scripts (tools/mapimport/materials.py, put in place
# of their "# ---- materials.py" line by embed.py): the folder, and the materials built from the
# .materials.json sidecar (MaterialDescription.h describes its fields)
def script_dir(names):
    """The export folder (the model or map file, its materials and images are named relative to
    it): this script's folder when Blender runs it as a file (blender --python, GrayWolf's Open in
    Blender); from the Text Editor, __file__ is only the text block's name, its file is the block's
    filepath. NAMES: the files one of which must be there."""
    f = globals().get("__file__") or ""
    paths = [f]
    try:
        sd = bpy.context.space_data
        if sd is not None and sd.type == "TEXT_EDITOR" and sd.text is not None:
            paths.append(sd.text.filepath)
    except AttributeError:
        pass
    paths += [t.filepath for t in bpy.data.texts if t.filepath and os.path.basename(bpy.path.abspath(t.filepath)) == os.path.basename(f)]
    paths += [t.filepath for t in bpy.data.texts if t.filepath]
    for p in paths:
        if p:
            d = os.path.dirname(os.path.abspath(bpy.path.abspath(p)))
            if any(os.path.isfile(os.path.join(d, n)) for n in names):
                return d
    raise FileNotFoundError(
        "GrayWolf: %s is not beside this script (%s). Open the script from the export folder "
        "(Text Editor > Open, then Run Script), or use GrayWolf's Open in Blender." % (" or ".join(names), f or "unsaved text"))


def new_material(name):
    mat = bpy.data.materials.new(name)
    if bpy.app.version < (5, 0, 0):   # always on from 5.0 (the property is deprecated)
        mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes.get("Principled BSDF")
    return mat, nt, bsdf


def set_alpha(mat, nt, bsdf, color, src, alpha=None):
    """The .glb material's alphaMode: MASK cut out at alphaCutoff, BLEND blended. The alpha is
    ALPHA (a socket) when given, else the colour map's."""
    mode = src.get("alphaMode", "OPAQUE")
    if (color is None and alpha is None) or mode == "OPAQUE":
        return
    if alpha is None:
        alpha = color.outputs["Alpha"]
    if mode == "MASK":
        cut = nt.nodes.new("ShaderNodeMath")
        cut.operation = "GREATER_THAN"
        cut.inputs[1].default_value = src.get("alphaCutoff", 0.5)
        cut.location = (-250, 450)
        nt.links.new(alpha, cut.inputs[0])
        alpha = cut.outputs[0]
    nt.links.new(alpha, bsdf.inputs["Alpha"])
    if hasattr(mat, "surface_render_method"):   # Blender 4.2 and later
        mat.surface_render_method = "BLENDED" if mode == "BLEND" else "DITHERED"
    try:   # what the Workbench (solid view) reads, and EEVEE before 4.2
        mat.blend_method = "BLEND" if mode == "BLEND" else "CLIP"
        mat.alpha_threshold = src.get("alphaCutoff", 0.5)
    except (AttributeError, TypeError):
        pass


FILES = {}
CHANNEL = {"r": 0, "g": 1, "b": 2, "a": 3}
UNREAD = {}   # the images Blender could not read, by file: why (the summary lists them)
# a model's import decodes the DDS formats Blender cannot read and checks that each image decoded;
# a map's (thousands of images, its signed ones written unsigned under textures/) sets it False
DECODE = True
# the DDS formats Blender cannot read (DXGI numbers): R8G8_SNORM, R8_UNORM (one-channel maps),
# BC4_SNORM, BC5_SNORM (Ghosts' normals); any other one it fails on is caught after loading
DDS_UNREAD = {51: "R8G8_SNORM", 61: "R8_UNORM", 81: "BC4_SNORM", 84: "BC5_SNORM"}


def dds_format(path):
    """A DDS file's DXGI format number, or 0 (not a DX10 DDS)."""
    try:
        with open(path, "rb") as f:
            head = f.read(132)
        return struct.unpack_from("<I", head, 128)[0] if head[84:88] == b"DX10" else 0
    except (OSError, struct.error):
        return 0


def blender_reads(path):
    """False for the DDS formats Blender cannot read."""
    return not path.lower().endswith(".dds") or dds_format(path) not in DDS_UNREAD


def bc4(b, signed):
    """BC4 blocks (n, 8 bytes) decoded: (n, 16) values, 0..1 (-1..1 signed), texels row by row."""
    e = (np.maximum(b[:, :2].view(np.int8).astype(np.float32), -127.0) / 127.0) if signed else b[:, :2] / np.float32(255.0)
    r0, r1 = e[:, 0:1], e[:, 1:2]
    i7, i5 = np.arange(1, 7, dtype=np.float32), np.arange(1, 5, dtype=np.float32)
    eight = np.concatenate([r0, r1, ((7 - i7) * r0 + i7 * r1) / 7], axis=1)
    six = np.concatenate([r0, r1, ((5 - i5) * r0 + i5 * r1) / 5, np.full_like(r0, -1.0 if signed else 0.0), np.ones_like(r0)], axis=1)
    pal = np.where(r0 > r1, eight, six)
    bits = np.zeros((len(b), 8), np.uint8)
    bits[:, :6] = b[:, 2:8]
    sel = (bits.view("<u8") >> (np.arange(16, dtype=np.uint64) * np.uint64(3))) & np.uint64(7)
    return np.take_along_axis(pal, sel.astype(np.int64), axis=1)


def decode_dds(path):
    """The DDS formats Blender cannot read, decoded here (mip 0) as a packed image (it is saved in
    the .blend): one channel as grey, two as red and green; signed values v as v * 0.5 + 0.5, as
    the normal maps' unsigned encoding reads them."""
    fmt = dds_format(path)
    if fmt not in DDS_UNREAD:
        return None
    with open(path, "rb") as f:
        data = f.read()
    h, w = struct.unpack_from("<II", data, 12)
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    raw = np.frombuffer(data, np.uint8, offset=148)
    if fmt in (81, 84):   # BC4, BC5: 8 bytes a channel a block
        k = 1 if fmt == 81 else 2
        blocks = raw[:bw * bh * 8 * k].reshape(-1, k, 8)
        ch = [bc4(np.ascontiguousarray(blocks[:, c]), True).reshape(bh, bw, 4, 4).transpose(0, 2, 1, 3).reshape(bh * 4, bw * 4)[:h, :w]
              for c in range(k)]
    else:   # R8_UNORM, R8G8_SNORM
        k = 1 if fmt == 61 else 2
        px = raw[:w * h * k].reshape(h, w, k)
        ch = [px[:, :, c] / np.float32(255.0) if fmt == 61 else np.maximum(px[:, :, c].view(np.int8).astype(np.float32), -127.0) / 127.0
              for c in range(k)]
    signed = fmt != 61
    ch = [c * 0.5 + 0.5 if signed else c for c in ch]
    rgba = np.ones((h, w, 4), np.float32)
    if k == 1:
        rgba[:, :, 0] = rgba[:, :, 1] = rgba[:, :, 2] = ch[0]
    else:
        rgba[:, :, 0], rgba[:, :, 1], rgba[:, :, 2] = ch[0], ch[1], 0.5 if signed else 0.0
    img = bpy.data.images.new(os.path.splitext(os.path.basename(path))[0], w, h, alpha=False)
    img.pixels.foreach_set(rgba[::-1].ravel())   # Blender's rows bottom up
    img.pack()
    return img


def readable_copy(path):
    """PATH, else a copy Blender reads: GrayWolf's under textures/ (a map's: unsigned v * 0.5 + 0.5
    for a signed DDS, or smaller with mips), or the same image beside it as PNG, TGA or TIFF (an
    export in that image format); None when there is none."""
    if not path.lower().endswith(".dds"):
        return path
    stem = os.path.splitext(path)[0]
    for p in [os.path.join("textures", os.path.basename(path))] + [stem + e for e in (".png", ".tga", ".tiff", ".tif")]:
        if os.path.exists(os.path.join(DIR, p)):
            return p
    return path if blender_reads(os.path.join(DIR, path)) else None


def file_image(path, color):
    key = (path, color)
    if key not in FILES:
        img = None
        use = readable_copy(path)
        if use is not None and not os.path.exists(os.path.join(DIR, use)):
            # not written by the export (a "$" engine image the zone holds no pixels for)
            UNREAD[path] = "missing"
            FILES[key] = None
            return None
        try:
            if use is None and DECODE:
                img = decode_dds(os.path.join(DIR, path))
            elif use is not None:
                # a data map gets its own image (its colour space differs from a colour map's)
                img = bpy.data.images.load(os.path.join(DIR, use), check_existing=color)
            if img is None:
                UNREAD[path] = "DDS %s" % DDS_UNREAD.get(dds_format(os.path.join(DIR, path)), "format")
            # loaded lazily: a file Blender cannot decode has no pixels (asked only by a model's
            # import: a map's thousands of images would all be decoded during it)
            elif DECODE and img.size[0] == 0:
                UNREAD[path] = "not decoded" if os.path.exists(os.path.join(DIR, use)) else "missing"
                bpy.data.images.remove(img)
                img = None
            elif not color:
                img.colorspace_settings.is_data = True
                img.alpha_mode = "CHANNEL_PACKED"
        except Exception as e:
            UNREAD[path] = str(e)
            img = None
        FILES[key] = img
    return FILES[key]


def alpha_masks(img, cutoff):
    """Whether a colour map's alpha masks something (the sidecar's "auto" alpha), as GrayWolf's
    preview judges it (AlphaRule.h): 2% of the texels clear, 80% clear or opaque; read on a copy
    scaled down to 256 texels a side."""
    if img is None or img.channels < 4 or img.size[0] == 0:
        return False
    small = img.copy()
    try:
        small.scale(min(256, img.size[0]), min(256, img.size[1]))
        px = np.empty(small.size[0] * small.size[1] * 4, np.float32)
        small.pixels.foreach_get(px)
        a = px[3::4]
        clear, solid = (a < 0.1).sum(), (a > 0.9).sum()
        return clear * 50 >= a.size and (clear + solid) * 5 >= a.size * 4
    except Exception:
        return False
    finally:
        bpy.data.images.remove(small)


def sidecar_alpha(d, albedo):
    """A sidecar material's alpha as a glTF alphaMode: cutout, blend, or auto (cut out when its
    colour map's alpha masks something)."""
    a = d.get("alpha", "opaque")
    cut = d.get("alpha_cutoff", 0.5)
    if a == "cutout" or (a == "auto" and albedo is not None and alpha_masks(albedo.image, cut)):
        return {"alphaMode": "MASK", "alphaCutoff": cut}
    if a == "blend":
        return {"alphaMode": "BLEND"}
    return {"alphaMode": "OPAQUE"}


def tex_node(nt, m, color, y, vec=None):
    img = file_image(m["path"], color) if m and m.get("path") else None
    if img is None:
        return None
    n = nt.nodes.new("ShaderNodeTexImage")
    n.image = img
    n.location = (-700, y)
    if vec is not None:
        nt.links.new(vec, n.inputs["Vector"])
    return n


def channel(nt, node, name, y):
    ch = CHANNEL.get(str(name).lower(), 0)
    if ch == 3:
        return node.outputs["Alpha"]
    sep = nt.nodes.new("ShaderNodeSeparateColor")
    sep.location = (-450, y)
    nt.links.new(node.outputs["Color"], sep.inputs[0])
    return sep.outputs[ch]


def feed(nt, sock, v):
    if isinstance(v, (int, float, tuple, list)):
        sock.default_value = v
    else:
        nt.links.new(v, sock)


def fmath(nt, op, a, b=None, c=None, y=0, clamp=False):
    """A Math node on sockets or numbers."""
    m = nt.nodes.new("ShaderNodeMath")
    m.operation = op
    m.use_clamp = clamp
    m.location = (-250, y)
    for i, v in enumerate((a, b, c)):
        if v is not None:
            feed(nt, m.inputs[i], v)
    return m.outputs[0]


def one_minus(nt, out, y):
    return fmath(nt, "SUBTRACT", 1.0, out, y=y)


def vmath(nt, op, a, b=None, c=None, y=0, scale=None):
    """A Vector Math node on sockets or 3-tuples (SCALE: its factor, a socket or a number)."""
    m = nt.nodes.new("ShaderNodeVectorMath")
    m.operation = op
    m.location = (-250, y)
    for i, v in enumerate((a, b, c)):
        if v is not None:
            feed(nt, m.inputs[i], v)
    if scale is not None:
        feed(nt, m.inputs["Scale"], scale)
    return m.outputs[0]


def mix(nt, kind, fac, a, b, y=0):
    """lerp(a, b, fac): kind FLOAT, VECTOR or RGBA."""
    m = nt.nodes.new("ShaderNodeMix")
    m.data_type = kind
    m.location = (-250, y)
    i = {"FLOAT": 2, "VECTOR": 4, "RGBA": 6}[kind]
    feed(nt, m.inputs[0], fac)
    feed(nt, m.inputs[i], a)
    feed(nt, m.inputs[i + 1], b)
    return m.outputs[{"FLOAT": 0, "VECTOR": 1, "RGBA": 2}[kind]]


def rgba(c):
    return tuple(c[:3]) + (1.0,)


def flip_v(nt, vec, y=0):
    """Blender's uv (v up) to the game's (v down), and back: (u, 1 - v)."""
    return vmath(nt, "MULTIPLY_ADD", vec, (1.0, -1.0, 1.0), (0.0, 1.0, 0.0), y)


def layer_uv(nt, uv, scale, rotation, y):
    """uv' = (R(-rotation) (uv - 0.5) + 0.5) * scale in the game's uv (the preview's LayerUV)."""
    g = flip_v(nt, uv, y)
    if rotation:
        r = nt.nodes.new("ShaderNodeVectorRotate")
        r.rotation_type = "Z_AXIS"
        r.inputs["Center"].default_value = (0.5, 0.5, 0.0)
        r.inputs["Angle"].default_value = -rotation
        r.location = (-900, y)
        nt.links.new(g, r.inputs["Vector"])
        g = r.outputs[0]
    return flip_v(nt, vmath(nt, "MULTIPLY", g, (scale[0], scale[1], 1.0), y=y), y)


def normal_xy(nt, m, y, vec=None):
    """A normal map's (x, y, 0) in -1..1 as the game reads it (x in r, or in a for ag), or None."""
    n = tex_node(nt, m, False, y, vec)
    if n is None:
        return None
    xy = n.outputs["Color"]
    if m.get("encoding") == "ag":
        sep = nt.nodes.new("ShaderNodeSeparateColor")
        comb = nt.nodes.new("ShaderNodeCombineXYZ")
        nt.links.new(n.outputs["Color"], sep.inputs[0])
        nt.links.new(n.outputs["Alpha"], comb.inputs[0])
        nt.links.new(sep.outputs[1], comb.inputs[1])
        xy = comb.outputs[0]
    return vmath(nt, "MULTIPLY_ADD", xy, (2.0, 2.0, 0.0), (-1.0, -1.0, 0.0), y)


def normal_out(nt, bsdf, xy, flip_y, y):
    """(x, y) in -1..1 as Blender's tangent normal: OpenGL green (a DirectX map's y negated), z
    rebuilt by the node's normalise."""
    c = vmath(nt, "MULTIPLY_ADD", xy, (0.5, 0.5 if flip_y else -0.5, 0.0), (0.5, 0.5, 1.0), y)
    nm = nt.nodes.new("ShaderNodeNormalMap")
    nm.location = (-100, y)
    nt.links.new(c, nm.inputs["Color"])
    nt.links.new(nm.outputs[0], bsdf.inputs["Normal"])


def roughness_of(nt, m, value, y, vec=None):
    """A roughness map's channel (1 - it for gloss), else the value."""
    r = tex_node(nt, m, False, y, vec)
    if r is None:
        return value
    out = channel(nt, r, m.get("channel", "r"), y)
    return one_minus(nt, out, y) if m.get("invert") else out


def layered(nt, d, uv, base_rough):
    """The layers: each colour tinted by its alpha, an upper layer over what is below by its mask,
    detail normals adding their xy to the base normal's (the sidecar's "layers", described in
    GrayWolf's MaterialDescription.h)."""
    color = rough = det = first = None
    for i, Ly in enumerate(d["layers"]):
        y = 900 - 700 * i
        luv = layer_uv(nt, uv, Ly.get("uv_scale", [1, 1]), Ly.get("uv_rotation", 0.0), y)
        a = tex_node(nt, Ly.get("albedo"), True, y, luv)
        c = (0.8, 0.8, 0.8, 1.0)
        if a is not None:
            first = first or a
            tint = mix(nt, "RGBA", a.outputs["Alpha"], (1.0, 1.0, 1.0, 1.0), rgba(Ly.get("tint", [1, 1, 1])), y)
            c = vmath(nt, "MULTIPLY", a.outputs["Color"], tint, y=y)
        rv = Ly.get("roughness_value", -1.0)
        r = roughness_of(nt, Ly.get("roughness"), rv if rv >= 0 else base_rough, y - 200, luv)
        n = normal_xy(nt, Ly.get("normal"), y - 400, luv)
        n = (0.0, 0.0, 0.0) if n is None else vmath(nt, "SCALE", n, y=y - 400, scale=Ly.get("normal_scale", 1.0))
        if color is None or "mask" not in Ly:
            color, rough, det = c, r, n
            continue
        mk = Ly["mask"]
        muv = vmath(nt, "MULTIPLY", flip_v(nt, uv, y), tuple(mk.get("uv_scale", [1, 1])) + (1.0,), y=y)
        mt = tex_node(nt, mk, False, y + 200, flip_v(nt, muv, y))
        m = channel(nt, mt, mk.get("channel", "r"), y + 200) if mt else 0.0
        color = mix(nt, "RGBA", m, color, c, y)
        rough = mix(nt, "FLOAT", m, rough, r, y - 200)
        det = mix(nt, "VECTOR", m, det, n, y - 400)
    return color, rough, det, first


def tinted(nt, d, color, uv):
    """color_tint (rgb *= lerp(1, color, mask)), then tint (rgb *= G(u, v), v the game's v moved
    by scale * (2 * offset - 1)), as the preview draws them."""
    ct = d.get("color_tint")
    mt = tex_node(nt, ct.get("mask"), False, 700) if ct else None
    if mt is not None:
        m = channel(nt, mt, ct["mask"].get("channel", "r"), 700)
        color = vmath(nt, "MULTIPLY", color, mix(nt, "RGBA", m, (1.0, 1.0, 1.0, 1.0), rgba(ct.get("color", [1, 1, 1])), 700), y=700)
    t = d.get("tint")
    if not t or not t.get("path") or uv is None:
        return color
    sep = nt.nodes.new("ShaderNodeSeparateXYZ")
    nt.links.new(uv, sep.inputs[0])
    v = one_minus(nt, sep.outputs[1], 1100)
    off = t.get("offset")
    ot = tex_node(nt, off, False, 1300) if off else None
    if ot is not None:
        o = fmath(nt, "MULTIPLY_ADD", channel(nt, ot, off.get("channel", "r"), 1300), 2.0, -1.0, y=1300)
        v = fmath(nt, "MULTIPLY_ADD", o, off.get("scale", 0.0), v, y=1100)
    v = one_minus(nt, fmath(nt, "ADD", v, 0.0, y=1100, clamp=True), 1100)
    comb = nt.nodes.new("ShaderNodeCombineXYZ")
    comb.inputs[0].default_value = t.get("u", 0.5)
    nt.links.new(v, comb.inputs[1])
    g = tex_node(nt, t, True, 1100, comb.outputs[0])
    if g is None:
        return color
    g.extension = "EXTEND"
    if t.get("channel", "rgb") == "rgb":
        gc = g.outputs["Color"]
    else:
        gc = vmath(nt, "SCALE", tuple(t.get("color", [1, 1, 1])), y=1100, scale=channel(nt, g, t["channel"], 1100))
    return vmath(nt, "MULTIPLY", color, gc, y=1100)


def sidecar_material(name, d, src):
    mat, nt, bsdf = new_material(name)
    L = nt.links
    base_rough = d.get("roughness_value", 0.6)
    bsdf.inputs["Roughness"].default_value = base_rough
    bsdf.inputs["Metallic"].default_value = d.get("metalness_value", 0.0)
    uv = None
    if d.get("layers") or d.get("tint") or d.get("alpha_map"):
        uvn = nt.nodes.new("ShaderNodeUVMap")
        uvn.location = (-1300, 0)
        uv = uvn.outputs["UV"]
    gamma = (d.get("albedo") or {}).get("gamma", 0.0)
    # a colour map that is not sRGB (Black Ops, BO2: c^2) is read as data and raised to its gamma
    albedo = tex_node(nt, d.get("albedo"), not gamma, 300)
    color = albedo.outputs["Color"] if albedo else None
    if color is not None and gamma:
        g = nt.nodes.new("ShaderNodeGamma")
        g.location = (-450, 300)
        g.inputs["Gamma"].default_value = gamma
        nt.links.new(color, g.inputs["Color"])
        color = g.outputs[0]
    rough = det = None
    if d.get("layers"):
        color, rough, det, first = layered(nt, d, uv, base_rough)
        albedo = albedo or first
    if color is not None and not isinstance(color, tuple) and (d.get("tint") or d.get("color_tint")):
        color = tinted(nt, d, color, uv)
    # the occlusion darkens the colour (Principled has no occlusion input)
    o = tex_node(nt, d.get("occlusion"), False, 500)
    if o is not None and color is not None and not isinstance(color, tuple):
        ao = channel(nt, o, d["occlusion"].get("channel", "r"), 500)
        color = mix(nt, "RGBA", ao, (0.0, 0.0, 0.0, 1.0), color, 500)
    if color is not None:
        feed(nt, bsdf.inputs["Base Color"], color)
    # metal workflow: the metalness map; else a specular map, its F0 as Specular IOR Level
    # (F0 = 0.08 * level), a pre-PBR one's colour times its scale
    mt = tex_node(nt, d.get("metalness"), False, -900)
    if mt is not None:
        feed(nt, bsdf.inputs["Metallic"], channel(nt, mt, d["metalness"].get("channel", "r"), -900))
    sp = tex_node(nt, d.get("specular"), True, -900) if mt is None else None
    if sp is not None and "Specular IOR Level" in bsdf.inputs:
        bw = nt.nodes.new("ShaderNodeRGBToBW")
        bw.location = (-450, -900)
        nt.links.new(sp.outputs["Color"], bw.inputs[0])
        scale = d["specular"].get("scale", 1.0) if d["specular"].get("legacy") else 1.0
        feed(nt, bsdf.inputs["Specular IOR Level"], fmath(nt, "MULTIPLY", bw.outputs[0], 12.5 * scale, y=-900, clamp=False))
    n = normal_xy(nt, d["normal"], -300) if d.get("normal") else None
    if det is not None and not isinstance(det, tuple):
        n = vmath(nt, "ADD", (0.0, 0.0, 0.0) if n is None else n, det, y=-300)
    if n is not None:
        normal_out(nt, bsdf, n, d.get("normal", {}).get("flip_y", False), -300)
    if rough is None:
        rough = roughness_of(nt, d.get("roughness"), base_rough, 0)
    feed(nt, bsdf.inputs["Roughness"], rough)
    e = tex_node(nt, d.get("emissive"), True, -600)
    if e:
        ec = d["emissive"].get("color")
        feed(nt, bsdf.inputs["Emission Color"], vmath(nt, "MULTIPLY", e.outputs["Color"], tuple(ec), y=-600) if ec else e.outputs["Color"])
        bsdf.inputs["Emission Strength"].default_value = 1.0
    if albedo is not None:
        nt.nodes.active = albedo   # the solid view's Texture colour: the albedo, not the last map made
    # a map setting the alpha in place of the colour map's: saturate((v - low) / (high - low)) at
    # uv * uv_scale (a reveal map's soft edge)
    alpha = None
    am = d.get("alpha_map")
    if am and uv is not None:
        muv = flip_v(nt, vmath(nt, "MULTIPLY", flip_v(nt, uv, -1200), tuple(am.get("uv_scale", [1, 1])) + (1.0,), y=-1200), -1200)
        at = tex_node(nt, am, False, -1200, muv)
        if at is not None:
            r = nt.nodes.new("ShaderNodeMapRange")
            r.location = (-450, -1200)
            r.clamp = True
            nt.links.new(channel(nt, at, am.get("channel", "r"), -1200), r.inputs["Value"])
            r.inputs["From Min"].default_value = am.get("low", 0.0)
            r.inputs["From Max"].default_value = am.get("high", 1.0)
            alpha = r.outputs["Result"]
    if src is None:   # a model's: the sidecar's alpha
        src = sidecar_alpha(d, albedo)
        if alpha is not None and src["alphaMode"] == "OPAQUE":
            src = {"alphaMode": "MASK", "alphaCutoff": d.get("alpha_cutoff", 0.5)}
    set_alpha(mat, nt, bsdf, albedo, src, alpha)
    return mat


def normal_only(d):
    """A decal whose colour map is its normal map (BO3's puddles, raindrops, running water: they
    change the gloss and the normals of what lies under them, not its colour)."""
    a, n = d.get("albedo") or {}, d.get("normal") or {}
    return bool(d.get("decal") or "decal" in str(d.get("techset", "")).lower()) and bool(a.get("path")) and a.get("path") == n.get("path")


def invisible_material(name):
    """Drawn as nothing: a normal map as a colour covered the streets in lilac sheets."""
    mat, nt, bsdf = new_material(name)
    bsdf.inputs["Alpha"].default_value = 0.0
    mat.diffuse_color = (0.8, 0.8, 0.8, 0.0)   # the solid view's colour
    if hasattr(mat, "surface_render_method"):
        mat.surface_render_method = "DITHERED"
    try:
        mat.blend_method = "CLIP"
        mat.shadow_method = "NONE"
    except (AttributeError, TypeError):
        pass
    mat["greyhoundx_note"] = "a decal that changes only the normals and the gloss of what it lies on: drawn as nothing"
    return mat


