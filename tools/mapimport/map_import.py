# The body of <map>_import.py, the Blender map import script Greyhound writes beside every map
# .glb: MapImportScript.cpp writes the parameters, then everything after the body marker below
# (embedded by embed.py). To test by hand, set the parameters here and run it beside a .glb.
GLB = "map.glb"
MATERIALS = ""
SKY = ""
SUN = None
ATMOSPHERE = False
INSTANCES = "collection"
# ---- body
import bpy, os, json, struct, time, math
import numpy as np
from urllib.parse import unquote
from mathutils import Matrix, Vector

DIR = os.path.dirname(os.path.abspath(__file__))
T0 = time.time()
SPENT = {"meshes": 0.0, "materials": 0.0}   # seconds, for the summary line


def read_glb(path):
    with open(path, "rb") as f:
        data = f.read()
    json_len = struct.unpack_from("<I", data, 12)[0]
    doc = json.loads(data[20:20 + json_len])
    off = 20 + json_len
    bin_len = struct.unpack_from("<I", data, off)[0] if off + 8 <= len(data) else 0
    return doc, memoryview(data)[off + 8:off + 8 + bin_len]


DOC, BIN = read_glb(os.path.join(DIR, GLB))
DTYPE = {5126: np.float32, 5121: np.uint8, 5123: np.uint16, 5125: np.uint32}
WIDTH = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def accessor(i):
    a = DOC["accessors"][i]
    v = DOC["bufferViews"][a["bufferView"]]
    n = WIDTH[a["type"]]
    arr = np.frombuffer(BIN, DTYPE[a["componentType"]], a["count"] * n, v.get("byteOffset", 0) + a.get("byteOffset", 0))
    return arr.reshape(-1, n) if n > 1 else arr


def gl_matrix(m):
    return Matrix([m[0:16:4], m[1:16:4], m[2:16:4], m[3:16:4]])


# glTF is Y up in metres, the game Z up in inches: the scene root converts. Blender is Z up, so
# the whole conversion is a uniform scale (baked into the vertices and placements).
nodes = DOC.get("nodes", [])
root = next((n for n in nodes if "children" in n and n.get("name") == "cod_to_gltf"), None)
Y_UP_TO_Z_UP = Matrix(((1, 0, 0, 0), (0, 0, -1, 0), (0, 1, 0, 0), (0, 0, 0, 1)))
TO_BLENDER = Y_UP_TO_Z_UP @ (gl_matrix(root["matrix"]) if root and "matrix" in root else Matrix.Identity(4))
SCALE = TO_BLENDER.to_scale()[0]
AXES = TO_BLENDER.to_3x3().normalized().to_4x4()


def placement(node):
    m = gl_matrix(node["matrix"]) if "matrix" in node else Matrix.Identity(4)
    m.translation = m.translation * SCALE
    return AXES @ m


# ---- images and materials (from the .glb: the base colour and the alpha mode)
IMAGES = {}


def image(index):
    if index in IMAGES:
        return IMAGES[index]
    img = None
    src = DOC["images"][index]
    try:
        if "uri" in src:
            img = bpy.data.images.load(os.path.join(DIR, unquote(src["uri"])), check_existing=True)   # a URI: %-encoded
        elif "bufferView" in src:
            v = DOC["bufferViews"][src["bufferView"]]
            data = bytes(BIN[v.get("byteOffset", 0):v.get("byteOffset", 0) + v["byteLength"]])
            img = bpy.data.images.new(src.get("name", "image"), 8, 8)
            img.source = "FILE"
            img.filepath = src.get("name", "image") + ".png"
            img.pack(data=data, data_len=len(data))
    except Exception as e:
        print("Greyhoundx: image %s: %s" % (src.get("uri", src.get("name")), e))
    IMAGES[index] = img
    return img


def new_material(name):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes.get("Principled BSDF")
    return mat, nt, bsdf


def gltf_material(index):
    src = DOC["materials"][index]
    mat, nt, bsdf = new_material(src.get("name", "material"))
    pbr = src.get("pbrMetallicRoughness", {})
    bsdf.inputs["Base Color"].default_value = pbr.get("baseColorFactor", [1, 1, 1, 1])
    bsdf.inputs["Roughness"].default_value = 0.8
    tex = DOC["textures"][pbr["baseColorTexture"]["index"]] if "baseColorTexture" in pbr else {}
    source = tex.get("source", tex.get("extensions", {}).get("MSFT_texture_dds", {}).get("source"))
    img = image(source) if source is not None else None
    if img:
        node = nt.nodes.new("ShaderNodeTexImage")
        node.image = img
        node.location = (-400, 200)
        nt.links.new(node.outputs["Color"], bsdf.inputs["Base Color"])
    set_alpha(mat, nt, bsdf, node if img else None, src)
    return mat


def set_alpha(mat, nt, bsdf, color, src):
    """The .glb material's alphaMode: MASK cut out at alphaCutoff, BLEND blended."""
    mode = src.get("alphaMode", "OPAQUE")
    if color is None or mode == "OPAQUE":
        return
    alpha = color.outputs["Alpha"]
    if mode == "MASK":
        cut = nt.nodes.new("ShaderNodeMath")
        cut.operation = "GREATER_THAN"
        cut.inputs[1].default_value = src.get("alphaCutoff", 0.5)
        cut.location = (-250, 450)
        nt.links.new(alpha, cut.inputs[0])
        alpha = cut.outputs[0]
    nt.links.new(alpha, bsdf.inputs["Alpha"])
    mat.surface_render_method = "BLENDED" if mode == "BLEND" else "DITHERED"
    try:   # what the Workbench (solid view) reads
        mat.blend_method = "BLEND" if mode == "BLEND" else "CLIP"
        mat.alpha_threshold = src.get("alphaCutoff", 0.5)
    except (AttributeError, TypeError):
        pass


# ---- materials from the sidecar (the game's own image files, DDS included: Blender reads them),
# for the textures the .glb does not carry (normal, roughness, emission)
SIDECAR = {}
if MATERIALS:
    try:
        with open(os.path.join(DIR, MATERIALS), encoding="utf-8") as f:
            SIDECAR = {m["name"]: m for m in json.load(f).get("materials", [])}
    except Exception as e:
        print("Greyhoundx: %s: %s" % (MATERIALS, e))
FILES = {}
CHANNEL = {"r": 0, "g": 1, "b": 2, "a": 3}


def blender_reads(path):
    """False for the DDS formats Blender cannot read: signed BC4/BC5 and R8G8 (Ghosts' normals)."""
    if not path.lower().endswith(".dds"):
        return True
    try:
        with open(path, "rb") as f:
            head = f.read(132)
        return not (head[84:88] == b"DX10" and struct.unpack_from("<I", head, 128)[0] in (51, 81, 84))
    except OSError:
        return False


def file_image(path, color):
    key = (path, color)
    if key not in FILES:
        img = None
        # Greyhound's copy under textures/, when it wrote one: unsigned (v * 0.5 + 0.5) for a
        # signed DDS Blender cannot read, or smaller with mips (the map_textures setting)
        copy = os.path.join("textures", os.path.basename(path))
        if path.lower().endswith(".dds") and os.path.exists(os.path.join(DIR, copy)):
            path = copy
        if not blender_reads(os.path.join(DIR, path)):
            FILES[key] = None
            return None
        try:
            # a data map gets its own image (its colour space differs from a colour map's)
            img = bpy.data.images.load(os.path.join(DIR, path), check_existing=color)
            if not color:
                img.colorspace_settings.is_data = True
                img.alpha_mode = "CHANNEL_PACKED"
        except Exception as e:
            print("Greyhoundx: image %s: %s" % (path, e))
        FILES[key] = img
    return FILES[key]


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
    if d.get("layers") or d.get("tint"):
        uvn = nt.nodes.new("ShaderNodeUVMap")
        uvn.location = (-1300, 0)
        uv = uvn.outputs["UV"]
    albedo = tex_node(nt, d.get("albedo"), True, 300)
    color = albedo.outputs["Color"] if albedo else None
    rough = det = None
    if d.get("layers"):
        color, rough, det, first = layered(nt, d, uv, base_rough)
        albedo = albedo or first
    if color is not None and not isinstance(color, tuple) and (d.get("tint") or d.get("color_tint")):
        color = tinted(nt, d, color, uv)
    if color is not None:
        feed(nt, bsdf.inputs["Base Color"], color)
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
        L.new(e.outputs["Color"], bsdf.inputs["Emission Color"])
        bsdf.inputs["Emission Strength"].default_value = 1.0
    set_alpha(mat, nt, bsdf, albedo, src)
    return mat


MATERIALS_BY_INDEX = {}


def material(index):
    if index < 0:
        return None
    if index not in MATERIALS_BY_INDEX:
        t = time.time()
        name = DOC["materials"][index].get("name", "material")
        MATERIALS_BY_INDEX[index] = (sidecar_material(name, SIDECAR[name], DOC["materials"][index])
                                     if name in SIDECAR else gltf_material(index))
        SPENT["materials"] += time.time() - t
    return MATERIALS_BY_INDEX[index]


# ---- meshes: every primitive of a glTF mesh in one Blender mesh, a material slot each
def build_mesh(index):
    src = DOC["meshes"][index]
    pos, nrm, uv, col, idx, mat_of_tri, mats = [], [], [], [], [], [], []
    base = 0
    for p in src["primitives"]:
        a = p["attributes"]
        P = accessor(a["POSITION"])
        pos.append(P)
        nrm.append(accessor(a["NORMAL"]) if "NORMAL" in a else np.zeros_like(P))
        uv.append(accessor(a["TEXCOORD_0"]) if "TEXCOORD_0" in a else np.zeros((len(P), 2), np.float32))
        col.append(accessor(a["COLOR_0"]) if "COLOR_0" in a else np.full((len(P), 4), 255, np.uint8))
        I = accessor(p["indices"]).astype(np.int32) + base
        idx.append(I)
        mat_of_tri.append(np.full(len(I) // 3, len(mats), np.int32))
        mats.append(p.get("material", -1))
        base += len(P)
    pos = np.concatenate(pos) * SCALE
    tris = np.concatenate(idx).reshape(-1, 3)
    # a triangle using one vertex twice is not a face to Blender (it would break its normals)
    keep = (tris[:, 0] != tris[:, 1]) & (tris[:, 1] != tris[:, 2]) & (tris[:, 0] != tris[:, 2])
    idx = tris[keep].ravel()
    me = bpy.data.meshes.new(src.get("name", "mesh"))
    me.vertices.add(len(pos))
    me.vertices.foreach_set("co", pos.astype(np.float32).ravel())
    me.loops.add(len(idx))
    me.loops.foreach_set("vertex_index", idx)
    me.polygons.add(len(idx) // 3)
    me.polygons.foreach_set("loop_start", np.arange(0, len(idx), 3, dtype=np.int32))
    me.polygons.foreach_set("material_index", np.concatenate(mat_of_tri)[keep])
    uvs = np.concatenate(uv)[idx]
    uvs[:, 1] = 1.0 - uvs[:, 1]
    me.uv_layers.new(name="UVMap").uv.foreach_set("vector", uvs.astype(np.float32).ravel())
    c = np.concatenate(col)
    if (c != 255).any():
        me.color_attributes.new("Col", "BYTE_COLOR", "POINT").data.foreach_set(
            "color_srgb", (c.astype(np.float32) / 255.0).ravel())
    for m in mats:
        me.materials.append(material(m))
    me.validate(clean_customdata=False)   # duplicate faces and the like
    me.update()
    n = np.concatenate(nrm).astype(np.float32)
    me.normals_split_custom_set_from_vertices(n)
    return me


def collection(name, parent):
    c = bpy.data.collections.new(name)
    parent.children.link(c)
    return c


scene = bpy.context.scene
top = collection(os.path.splitext(GLB)[0], scene.collection)
world_col = collection("world", top)
placed_col = collection("placements", top)
lights_col = collection("lights", top)
models_col = collection("models", top)   # one collection per placed model, instanced; excluded
users = {}
for n in nodes:
    if "mesh" in n:
        users[n["mesh"]] = users.get(n["mesh"], 0) + 1
MESHES, PROTOS = {}, {}


def mesh(index):
    if index not in MESHES:
        t = time.time()
        MESHES[index] = build_mesh(index)
        SPENT["meshes"] += time.time() - t
    return MESHES[index]


def proto(index):
    if index not in PROTOS:
        me = mesh(index)
        c = collection(me.name, models_col)
        c.objects.link(bpy.data.objects.new(me.name, me))
        PROTOS[index] = c
    return PROTOS[index]


LIGHTS = DOC.get("extensions", {}).get("KHR_lights_punctual", {}).get("lights", [])
counts = {"objects": 0, "instances": 0, "empties": 0, "lights": 0}
GN_POINTS = {}
content = root["children"] if root else range(len(nodes))
for i in content:
    n = nodes[i]
    name = n.get("name", "node")
    m = placement(n)
    if "mesh" in n and INSTANCES == "geometry_nodes" and users[n["mesh"]] > 1:
        GN_POINTS.setdefault(n["mesh"], []).append(m)
        continue
    if "mesh" in n and INSTANCES == "collection" and users[n["mesh"]] > 1:
        ob = bpy.data.objects.new(name, None)
        ob.instance_type = "COLLECTION"
        ob.instance_collection = proto(n["mesh"])
        ob.empty_display_size = 0.25
        placed_col.objects.link(ob)
        counts["instances"] += 1
    elif "mesh" in n:
        ob = bpy.data.objects.new(name, mesh(n["mesh"]))
        (world_col if users[n["mesh"]] == 1 and m == Matrix.Identity(4) else placed_col).objects.link(ob)
        counts["objects"] += 1
    elif "KHR_lights_punctual" in n.get("extensions", {}):
        L = LIGHTS[n["extensions"]["KHR_lights_punctual"]["light"]]
        kind = {"directional": "SUN", "point": "POINT", "spot": "SPOT"}[L["type"]]
        ld = bpy.data.lights.new(L.get("name", name), kind)
        ld.color = L.get("color", (1, 1, 1))
        power = L.get("intensity", 1.0) / 683.0   # the glTF importer's photometric conversion
        ld.energy = power if kind == "SUN" else power * 4 * math.pi
        if kind == "SPOT":
            ld.spot_size = 2 * L.get("spot", {}).get("outerConeAngle", math.pi / 4)
            ld.spot_blend = 1.0
        if kind != "SUN" and L.get("range"):
            ld.use_custom_distance = True
            ld.cutoff_distance = L["range"] * SCALE
        ld.shadow_soft_size = 0
        ob = bpy.data.objects.new(name, ld)
        lights_col.objects.link(ob)
        counts["lights"] += 1
    else:
        ob = bpy.data.objects.new(name, None)
        ob.empty_display_size = 0.25
        placed_col.objects.link(ob)
        counts["empties"] += 1
    ob.matrix_world = m

def gn_group():
    g = bpy.data.node_groups.new("Greyhoundx placements", "GeometryNodeTree")
    g.interface.new_socket("Geometry", in_out="INPUT", socket_type="NodeSocketGeometry")
    g.interface.new_socket("Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
    N, L = g.nodes, g.links
    gi, go = N.new("NodeGroupInput"), N.new("NodeGroupOutput")
    info = N.new("GeometryNodeCollectionInfo")
    info.transform_space = "ORIGINAL"
    inst = N.new("GeometryNodeInstanceOnPoints")
    attr = N.new("GeometryNodeInputNamedAttribute")
    attr.data_type = "FLOAT4X4"
    attr.inputs["Name"].default_value = "placement"
    st = N.new("GeometryNodeSetInstanceTransform")
    L.new(gi.outputs["Geometry"], inst.inputs["Points"])
    L.new(info.outputs[0], inst.inputs["Instance"])
    L.new(inst.outputs[0], st.inputs["Instances"])
    L.new(attr.outputs[0], st.inputs["Transform"])
    L.new(st.outputs[0], go.inputs["Geometry"])
    return g


if GN_POINTS:
    group = gn_group()
    for mi, mats in GN_POINTS.items():
        c = proto(mi)
        pts = bpy.data.meshes.new(c.name + "_placements")
        pts.vertices.add(len(mats))
        pts.vertices.foreach_set("co", np.array([tuple(x.translation) for x in mats], np.float32).ravel())
        a = pts.attributes.new("placement", "FLOAT4X4", "POINT")
        a.data.foreach_set("value", np.array([[x[r][k] for k in range(4) for r in range(4)] for x in mats], np.float32).ravel())
        ob = bpy.data.objects.new(c.name, pts)
        md = ob.modifiers.new("placements", "NODES")
        md.node_group = group.copy()
        md.node_group.name = c.name
        md.node_group.nodes["Collection Info"].inputs["Collection"].default_value = c
        placed_col.objects.link(ob)
        counts["instances"] += len(mats)
# the model collections are only what the instances show
for lc in bpy.context.view_layer.layer_collection.children[top.name].children:
    if lc.collection == models_col:
        lc.exclude = True


def sky_strength(node):
    """An HDR (BC6H) sky is stored at the game's exposure, far below 1 (zm_factory: mean 0.0007):
    its emission is scaled so that its mean reads 0.25; an 8-bit sky keeps 1."""
    img = getattr(node, "image", None)
    if img is None or not img.is_float or img.size[0] == 0:
        return 1.0
    small = img.copy()
    small.scale(256, 128)
    px = np.empty(256 * 128 * 4, np.float32)
    small.pixels.foreach_get(px)
    bpy.data.images.remove(small)
    mean = float(px.reshape(-1, 4)[:, :3].mean())
    return 0.25 / mean if mean > 1e-6 else 1.0


def make_sky(obs):
    """The game draws its sky model around the camera, far away: here it is centred on the world
    and scaled to enclose it, lit by itself (its colour as emission) and casting no shadow."""
    world = [o for o in world_col.objects if o.type == "MESH" and o not in obs]
    if not obs or not world:
        return
    pts = [o.matrix_world @ Vector(c) for o in world for c in o.bound_box]
    lo = Vector([min(p[i] for p in pts) for i in range(3)])
    hi = Vector([max(p[i] for p in pts) for i in range(3)])
    for ob in obs:
        corners = [Vector(c) for c in ob.bound_box]
        radius = max(c.length for c in corners) or 1.0
        ob.scale = [2.0 * max((hi - lo).length / 2, 100.0) / radius] * 3
        ob.location = ((lo + hi) / 2).to_tuple()
        ob.visible_shadow = False
        for slot in ob.data.materials:
            if slot is None or slot.get("greyhoundx_sky"):
                continue
            slot["greyhoundx_sky"] = True
            nt = slot.node_tree
            bsdf = nt.nodes.get("Principled BSDF")
            link = next((l for l in nt.links if l.to_node == bsdf and l.to_socket.name == "Base Color"), None)
            if link:
                nt.links.new(link.from_socket, bsdf.inputs["Emission Color"])
                bsdf.inputs["Emission Strength"].default_value = sky_strength(link.from_node)
                nt.links.remove(link)
            bsdf.inputs["Base Color"].default_value = (0, 0, 0, 1)


SKY_OBJECTS = [o for o in world_col.objects if o.name.startswith("sky_")]
make_sky(SKY_OBJECTS)


def sun_and_atmosphere():
    """The map's sun (SUN: toward it, game axes; Blender's are the same, Z up) as a sun lamp, and,
    when the game draws its sky by scattering (ATMOSPHERE: no texture exists), Blender's physical
    sky with that sun in place of the untextured sky model."""
    if not SUN:
        return
    d = Vector(SUN).normalized()
    ld = bpy.data.lights.new("sun", "SUN")
    ld.energy = 3.0
    ld.angle = math.radians(0.5)
    ob = bpy.data.objects.new("sun", ld)
    ob.rotation_euler = (-d).to_track_quat("-Z", "Y").to_euler()
    lights_col.objects.link(ob)
    if not ATMOSPHERE:
        return
    w = scene.world or bpy.data.worlds.new("World")
    scene.world = w
    w.use_nodes = True
    nt = w.node_tree
    for nd in list(nt.nodes):
        nt.nodes.remove(nd)
    sky = nt.nodes.new("ShaderNodeTexSky")
    sky.sun_elevation = math.asin(max(-1.0, min(1.0, d.z)))
    sky.sun_rotation = math.atan2(d.x, d.y)   # Blender's sky: rotation 0 puts the sun toward +Y
    bg = nt.nodes.new("ShaderNodeBackground")
    bg.inputs["Strength"].default_value = 0.15
    out = nt.nodes.new("ShaderNodeOutputWorld")
    nt.links.new(sky.outputs["Color"], bg.inputs["Color"])
    nt.links.new(bg.outputs["Background"], out.inputs["Surface"])
    for o in SKY_OBJECTS:   # the untextured sky model would hide the physical sky
        o.hide_render = o.hide_viewport = True


sun_and_atmosphere()

if SKY:
    w = scene.world or bpy.data.worlds.new("World")
    scene.world = w
    w.use_nodes = True
    nt = w.node_tree
    for nd in list(nt.nodes):
        nt.nodes.remove(nd)
    bg = nt.nodes.new("ShaderNodeBackground")
    env = nt.nodes.new("ShaderNodeTexEnvironment")
    env.image = bpy.data.images.load(os.path.join(DIR, SKY))
    out = nt.nodes.new("ShaderNodeOutputWorld")
    nt.links.new(env.outputs["Color"], bg.inputs["Color"])
    nt.links.new(bg.outputs["Background"], out.inputs["Surface"])


# ---- placements made real on demand (GN instances are not selectable one by one):
# greyhoundx_realize() and Object > Realize Greyhoundx Placements; also the text block
# greyhoundx_realize.py, registered when the .blend opens (if scripts may run).
REALIZE_CODE = r'''
import bpy
import numpy as np
from mathutils import Matrix, Vector


def is_scatter(ob):
    return (ob is not None and ob.type == "MESH" and "placement" in ob.data.attributes
            and any(m.type == "NODES" for m in ob.modifiers))


def scatter_model(ob):
    """the model a scatter object places: its node group's collection, that collection's object"""
    for m in ob.modifiers:
        if m.type == "NODES" and m.node_group and "Collection Info" in m.node_group.nodes:
            c = m.node_group.nodes["Collection Info"].inputs["Collection"].default_value
            return c.objects[0] if c and len(c.objects) else None
    return None


def placements(ob):
    a = ob.data.attributes["placement"]
    v = np.empty(len(a.data) * 16, np.float32)
    a.data.foreach_get("value", v)
    return [ob.matrix_world @ Matrix(m.reshape(4, 4).T.tolist()) for m in v.reshape(-1, 16)]


def keep_only(ob, keep):
    """the scatter's points reduced to the indices in keep (the object removed when none is left)"""
    old = ob.data
    if not keep:
        bpy.data.objects.remove(ob)
        bpy.data.meshes.remove(old)
        return
    co = np.empty(len(old.vertices) * 3, np.float32)
    old.vertices.foreach_get("co", co)
    a = old.attributes["placement"]
    m = np.empty(len(a.data) * 16, np.float32)
    a.data.foreach_get("value", m)
    k = np.array(keep)
    new = bpy.data.meshes.new(old.name)
    new.vertices.add(len(k))
    new.vertices.foreach_set("co", co.reshape(-1, 3)[k].ravel())
    new.attributes.new("placement", "FLOAT4X4", "POINT").data.foreach_set("value", m.reshape(-1, 16)[k].ravel())
    ob.data = new
    bpy.data.meshes.remove(old)


def greyhoundx_realize(objects=None, box=None, collection="realized placements"):
    """Real objects for Greyhoundx's Geometry Nodes placements: every placement of the scatter
    objects given (default: the selected ones), or, with box = (min corner, max corner) in world
    space, those of every scatter object whose origin is inside it. They leave the scatter (no
    double), share the model's mesh and go to one collection. Returns the new objects."""
    scene = bpy.context.scene
    if box is not None:
        objects = [o for o in scene.objects if is_scatter(o)]
        lo, hi = Vector(box[0]), Vector(box[1])
    elif objects is None:
        objects = [o for o in bpy.context.selected_objects if is_scatter(o)]
    out = bpy.data.collections.get(collection)
    if out is None:
        out = bpy.data.collections.new(collection)
        scene.collection.children.link(out)
    made = []
    for ob in list(objects):
        model = scatter_model(ob)
        if model is None:
            continue
        mats = placements(ob)
        take = [i for i, m in enumerate(mats) if box is None or all(lo[j] <= m.translation[j] <= hi[j] for j in range(3))]
        for i in take:
            r = bpy.data.objects.new(model.name, model.data)
            r.matrix_world = mats[i]
            out.objects.link(r)
            made.append(r)
        if take:
            chosen = set(take)
            keep_only(ob, [i for i in range(len(mats)) if i not in chosen])
    return made


class GREYHOUNDX_OT_realize_placements(bpy.types.Operator):
    """Turn Greyhoundx's Geometry Nodes placements into real, selectable objects: all those of the
    selected scatter objects, or those inside the bounds of the other selected objects (a box)"""
    bl_idname = "greyhoundx.realize_placements"
    bl_label = "Realize Greyhoundx Placements"
    bl_options = {"REGISTER", "UNDO"}
    mode: bpy.props.EnumProperty(items=[
        ("SELECTED", "Selected models", "Every placement of the selected scatter objects"),
        ("BOX", "Inside the selection's bounds", "Placements of any model inside the bounds of the selected non-scatter objects")])

    def execute(self, context):
        if self.mode == "BOX":
            boxes = [o for o in context.selected_objects if not is_scatter(o)]
            if not boxes:
                self.report({"WARNING"}, "select an object whose bounds enclose the placements (a cube)")
                return {"CANCELLED"}
            pts = [o.matrix_world @ Vector(c) for o in boxes for c in o.bound_box]
            box = ([min(p[i] for p in pts) for i in range(3)], [max(p[i] for p in pts) for i in range(3)])
            made = greyhoundx_realize(box=box)
        else:
            made = greyhoundx_realize()
        for o in context.selected_objects:
            o.select_set(False)
        for o in made:
            o.select_set(True)
        self.report({"INFO"}, "%d placements realized" % len(made))
        return {"FINISHED"}


def _menu(self, context):
    self.layout.operator_menu_enum(GREYHOUNDX_OT_realize_placements.bl_idname, "mode")


def register():
    try:
        bpy.utils.unregister_class(bpy.types.GREYHOUNDX_OT_realize_placements)
    except (AttributeError, RuntimeError):
        pass
    bpy.utils.register_class(GREYHOUNDX_OT_realize_placements)
    old = bpy.app.driver_namespace.get("greyhoundx_menu")
    if old:
        bpy.types.VIEW3D_MT_object.remove(old)
    bpy.types.VIEW3D_MT_object.append(_menu)
    bpy.app.driver_namespace["greyhoundx_menu"] = _menu
    bpy.app.driver_namespace["greyhoundx_realize"] = greyhoundx_realize


register()
'''
if GN_POINTS:
    _txt = bpy.data.texts.get("greyhoundx_realize.py") or bpy.data.texts.new("greyhoundx_realize.py")
    _txt.from_string(REALIZE_CODE)
    _txt.use_module = True
    _ns = {"__name__": "greyhoundx_realize"}
    exec(compile(REALIZE_CODE, "greyhoundx_realize.py", "exec"), _ns)
    greyhoundx_realize = _ns["greyhoundx_realize"]

print("Greyhoundx: imported %s in %.1fs (meshes %.1fs, materials %.1fs): %d objects, %d instances of %d models, "
      "%d lights, %d empties, %d materials" % (
          GLB, time.time() - T0, SPENT["meshes"] - SPENT["materials"], SPENT["materials"], counts["objects"],
          counts["instances"], len(PROTOS), counts["lights"], counts["empties"], len(MATERIALS_BY_INDEX)))
