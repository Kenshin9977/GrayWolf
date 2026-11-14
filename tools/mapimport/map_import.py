# The body of <map>_import.py, the Blender map import script Greyhound writes beside every map
# .glb: MapImportScript.cpp writes the parameters, then everything after the body marker below
# (embedded by embed.py, which puts materials.py, shared with the model import script, in place of
# the "# ---- materials.py" line). To test by hand, export a map and edit the script written beside it.
GLB = "map.glb"
MATERIALS = ""
SKY = ""
SUN = None
ATMOSPHERE = False
PROPS = "fast"
# ---- body
import bpy, os, sys, json, struct, time, math
import numpy as np
# the props (models placed more than once): "fast", one object per map cell placing them by Geometry
# Nodes (Object > GrayWolf: Make selection editable turns a region's into objects); "objects", one
# object per placement (linked duplicates, a collection per map cell); "collection", one collection
# instance per placement. Blender's command line may choose: -- ... --props objects. A script
# written before PROPS has INSTANCES ("geometry_nodes" is "fast").
_ARGS = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if "--props" in _ARGS[:-1]:
    PROPS = _ARGS[_ARGS.index("--props") + 1]
PROPS = globals().get("PROPS") or {"geometry_nodes": "fast"}.get(globals().get("INSTANCES"), globals().get("INSTANCES", "fast"))
if PROPS not in ("fast", "objects", "collection"):
    print("GrayWolf: PROPS = %r unknown, \"fast\" used" % PROPS)
    PROPS = "fast"
INSTANCES = {"fast": "geometry_nodes"}.get(PROPS, PROPS)
from urllib.parse import unquote
from mathutils import Matrix, Vector

# ---- materials.py


DIR = script_dir([GLB])
DECODE = False   # a map's thousands of images are not decoded during its import
T0 = time.time()
# Blender 4.0 and 4.1 have no 4x4 matrix attribute nor Set Instance Transform node (4.2): the
# placements are then stored as rotation and scale vectors
MATRIX_ATTR = bpy.app.version >= (4, 2, 0)
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
        nt.nodes.active = node   # what the solid view's Texture colour shows
    set_alpha(mat, nt, bsdf, node if img else None, src)
    return mat


# ---- materials from the sidecar (the game's own image files, DDS included: Blender reads them),
# for the textures the .glb does not carry (normal, roughness, emission)
SIDECAR = {}
if MATERIALS:
    try:
        with open(os.path.join(DIR, MATERIALS), encoding="utf-8") as f:
            SIDECAR = {m["name"]: m for m in json.load(f).get("materials", [])}
    except Exception as e:
        print("Greyhoundx: %s: %s" % (MATERIALS, e))
MATERIALS_BY_INDEX = {}


def material(index):
    if index < 0:
        return None
    if index not in MATERIALS_BY_INDEX:
        t = time.time()
        name = DOC["materials"][index].get("name", "material")
        d = SIDECAR.get(name)
        if d is not None and normal_only(d):
            MATERIALS_BY_INDEX[index] = invisible_material(name)
        else:
            MATERIALS_BY_INDEX[index] = (sidecar_material(name, d, DOC["materials"][index])
                                         if d is not None else gltf_material(index))
        SPENT["materials"] += time.time() - t
    return MATERIALS_BY_INDEX[index]


# a decal (the sidecar's flag, or a decal technique set) lies on another surface: the game draws it
# with a depth bias, Blender would make the two z-fight in stripes; it is lifted by DECAL_LIFT (m)
DECAL_LIFT = 0.004


def is_decal(index):
    if index < 0:
        return False
    d = SIDECAR.get(DOC["materials"][index].get("name", ""), {})
    return bool(d.get("decal")) or "decal" in str(d.get("techset", "")).lower()


# ---- meshes: a glTF mesh read as arrays (every primitive, its triangles numbering their glTF
# material), then one Blender mesh with a material slot per material used
ARRAYS = ("pos", "nrm", "uv", "col", "wts")


def gather(index):
    src = DOC["meshes"][index]
    pos, nrm, uv, col, wts, idx, tmat, lift = [], [], [], [], [], [], [], []
    base = 0
    for p in src["primitives"]:
        a = p["attributes"]
        P = accessor(a["POSITION"])
        pos.append(P)
        nrm.append(accessor(a["NORMAL"]) if "NORMAL" in a else np.zeros_like(P))
        uv.append(accessor(a["TEXCOORD_0"]) if "TEXCOORD_0" in a else np.zeros((len(P), 2), np.float32))
        col.append(accessor(a["COLOR_0"]) if "COLOR_0" in a else np.full((len(P), 4), 255, np.uint8))
        # a layered material's blend weights (G the second layer's, B the third's, A the fourth's)
        wts.append(accessor(a["_LAYER_WEIGHTS"]) if "_LAYER_WEIGHTS" in a else np.full((len(P), 4), 255, np.uint8))
        I = accessor(p["indices"]).astype(np.int32) + base
        idx.append(I)
        m = p.get("material", -1)
        tmat.append(np.full(len(I) // 3, m, np.int32))
        lift.append(np.full(len(P), DECAL_LIFT if is_decal(m) else 0.0, np.float32))
        base += len(P)
    g = {"pos": np.concatenate(pos).astype(np.float32) * np.float32(SCALE),
         "nrm": np.concatenate(nrm).astype(np.float32), "uv": np.concatenate(uv),
         "col": np.concatenate(col), "wts": np.concatenate(wts)}
    lift = np.concatenate(lift)
    if lift.any():
        g["pos"] = g["pos"] + g["nrm"] * lift[:, None]
    tris = np.concatenate(idx).reshape(-1, 3)
    # a triangle using one vertex twice is not a face to Blender (it would break its normals)
    keep = (tris[:, 0] != tris[:, 1]) & (tris[:, 1] != tris[:, 2]) & (tris[:, 0] != tris[:, 2])
    g["tris"], g["tmat"] = tris[keep], np.concatenate(tmat)[keep]
    return g


def subset(g, mask):
    """the triangles of g that mask keeps, with only the vertices they use"""
    tris = g["tris"][mask]
    used, inv = np.unique(tris.ravel(), return_inverse=True)
    out = {k: g[k][used] for k in ARRAYS}
    out["tris"], out["tmat"] = inv.reshape(-1, 3).astype(np.int32), g["tmat"][mask]
    return out


def concat(parts):
    if len(parts) == 1:
        return parts[0]
    out = {k: np.concatenate([p[k] for p in parts]) for k in ARRAYS + ("tmat",)}
    base = np.cumsum([0] + [len(p["pos"]) for p in parts[:-1]])
    out["tris"] = np.concatenate([p["tris"] + int(b) for p, b in zip(parts, base)])
    return out


def make_mesh(name, g, origin=None):
    """a Blender mesh of g, its vertices relative to origin (Blender units) when given"""
    pos = g["pos"] if origin is None else g["pos"] - np.asarray(origin, np.float32)
    mats, slot = np.unique(g["tmat"], return_inverse=True)
    idx = g["tris"].ravel()
    me = bpy.data.meshes.new(name)
    me.vertices.add(len(pos))
    me.vertices.foreach_set("co", pos.astype(np.float32).ravel())
    me.loops.add(len(idx))
    me.loops.foreach_set("vertex_index", idx)
    me.polygons.add(len(idx) // 3)
    me.polygons.foreach_set("loop_start", np.arange(0, len(idx), 3, dtype=np.int32))
    me.polygons.foreach_set("material_index", slot.ravel().astype(np.int32))
    uvs = g["uv"][idx]
    uvs[:, 1] = 1.0 - uvs[:, 1]
    me.uv_layers.new(name="UVMap").uv.foreach_set("vector", uvs.astype(np.float32).ravel())
    c = g["col"]
    if (c != 255).any():
        me.color_attributes.new("Col", "BYTE_COLOR", "POINT").data.foreach_set(
            "color_srgb", (c.astype(np.float32) / 255.0).ravel())
    w = g["wts"]
    if (w != 255).any():
        me.color_attributes.new("LayerWeights", "BYTE_COLOR", "POINT").data.foreach_set(
            "color_srgb", (w.astype(np.float32) / 255.0).ravel())   # the bytes as they are
    for m in mats:
        me.materials.append(material(int(m)))
    me.validate(clean_customdata=False)   # duplicate faces and the like
    me.update()
    if hasattr(me, "use_auto_smooth"):   # before 4.1, custom normals show only with it
        me.use_auto_smooth = True
    me.normals_split_custom_set_from_vertices(g["nrm"])
    return me


def build_mesh(index):
    return make_mesh(DOC["meshes"][index].get("name", "mesh"), gather(index))


def collection(name, parent):
    c = bpy.data.collections.new(name)
    parent.children.link(c)
    return c


scene = bpy.context.scene
top = collection(os.path.splitext(GLB)[0], scene.collection)
world_col = collection("world", top)
props_col = collection("props", top)       # Geometry Nodes placements, one object per map cell
placed_col = collection("placements", top)
lights_col = collection("lights", top)     # hidden in the viewport (its eye shows them)
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
    """The model's collection under 'models'. Its name starts with its number, zero-padded: the
    Collection Info node lists the children by name, and the cells pick a model by that number."""
    if index not in PROTOS:
        me = mesh(index)
        k = len(PROTOS)
        c = collection("%05d %s" % (k, me.name), models_col)
        c["greyhoundx_index"] = k
        c.objects.link(bpy.data.objects.new(me.name, me))
        PROTOS[index] = c
    return PROTOS[index]


def sky_surface(index):
    """a world surface drawn with a sky technique (IW's sky dome is world geometry, wc_sky)"""
    for p in DOC["meshes"][index]["primitives"]:
        m = p.get("material", -1)
        d = SIDECAR.get(DOC["materials"][m].get("name", ""), {}) if m >= 0 else {}
        if "sky" not in str(d.get("techset", "")).lower():
            return False
    return True


LIGHTS = DOC.get("extensions", {}).get("KHR_lights_punctual", {}).get("lights", [])
counts = {"objects": 0, "instances": 0, "empties": 0, "lights": 0, "cells": 0, "world": 0}
GN_POINTS = {}   # the props' placements by mesh ("fast" and "objects")
OBJ_NAMES = {}   # their node names ("objects")
WORLD = []   # the world surfaces (models placed once, at the origin), split by map cell below
content = root["children"] if root else range(len(nodes))
for i in content:
    n = nodes[i]
    name = n.get("name", "node")
    m = placement(n)
    if "mesh" in n and INSTANCES in ("geometry_nodes", "objects") and users[n["mesh"]] > 1:
        GN_POINTS.setdefault(n["mesh"], []).append(m)
        if INSTANCES == "objects":
            OBJ_NAMES.setdefault(n["mesh"], []).append(name)
        continue
    if "mesh" in n and INSTANCES == "collection" and users[n["mesh"]] > 1:
        ob = bpy.data.objects.new(name, None)
        ob.instance_type = "COLLECTION"
        ob.instance_collection = proto(n["mesh"])
        ob.empty_display_size = 0.25
        placed_col.objects.link(ob)
        counts["instances"] += 1
    elif "mesh" in n:
        world = users[n["mesh"]] == 1 and m == Matrix.Identity(4)
        if world and not name.startswith("sky_") and not sky_surface(n["mesh"]):
            WORLD.append(n["mesh"])
            continue
        # a model placed once is named after it (the placement's name, smodel_<n>, as a property)
        ob = bpy.data.objects.new(name if world else mesh(n["mesh"]).name, mesh(n["mesh"]))
        if not world:
            ob["placement"] = name
        elif sky_surface(n["mesh"]):
            ob.hide_select = True   # a sky dome over the map: any box selection would take it
        (world_col if world else placed_col).objects.link(ob)
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


def rotation_scale(m):
    """A placement as an Euler rotation and a scale (a mirror: the scale's x negated); its shear,
    which the game does not place, is lost."""
    r = m.to_3x3()
    s = Vector([c.length for c in r.col])
    if r.determinant() < 0:
        s.x = -s.x
    for k in range(3):
        if s[k] != 0:
            r.col[k] = r.col[k] / s[k]
    return r.to_euler(), s


def gn_group(models):
    """Instances every point of a cell: the model its 'model' attribute numbers (a child of the
    models collection), placed by its 'placement' matrix (Blender 4.2+) or 'rotation' and 'scale'."""
    g = bpy.data.node_groups.new("Greyhoundx placements", "GeometryNodeTree")
    g.interface.new_socket("Geometry", in_out="INPUT", socket_type="NodeSocketGeometry")
    g.interface.new_socket("Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
    N, L = g.nodes, g.links
    gi, go = N.new("NodeGroupInput"), N.new("NodeGroupOutput")
    info = N.new("GeometryNodeCollectionInfo")
    info.transform_space = "ORIGINAL"
    info.inputs["Collection"].default_value = models
    info.inputs["Separate Children"].default_value = True
    info.inputs["Reset Children"].default_value = True
    inst = N.new("GeometryNodeInstanceOnPoints")
    inst.inputs["Pick Instance"].default_value = True

    def attr(name, kind):
        a = N.new("GeometryNodeInputNamedAttribute")
        a.data_type = kind
        a.inputs["Name"].default_value = name
        return a.outputs["Attribute"]

    L.new(gi.outputs["Geometry"], inst.inputs["Points"])
    L.new(info.outputs[0], inst.inputs["Instance"])
    L.new(attr("model", "INT"), inst.inputs["Instance Index"])
    if MATRIX_ATTR:
        st = N.new("GeometryNodeSetInstanceTransform")
        L.new(inst.outputs[0], st.inputs["Instances"])
        L.new(attr("placement", "FLOAT4X4"), st.inputs["Transform"])
        L.new(st.outputs[0], go.inputs["Geometry"])
    else:
        rot = attr("rotation", "FLOAT_VECTOR")
        try:   # 4.0's rotation socket takes a rotation; the vector is an Euler
            e2r = N.new("FunctionNodeEulerToRotation")
            L.new(rot, e2r.inputs[0])
            rot = e2r.outputs[0]
        except RuntimeError:
            pass
        L.new(rot, inst.inputs["Rotation"])
        L.new(attr("scale", "FLOAT_VECTOR"), inst.inputs["Scale"])
        L.new(inst.outputs[0], go.inputs["Geometry"])
    return g


def grid_size(xy):
    """The map cell's side (m): about a sixteenth of the extent of xy (the props' positions, else
    the world's triangles), at least 16 m. The props and the world are cut by the same grid, so a
    box selection picks only the cells under it (one object per model or per material spread over
    the whole map: any box touching it took it all, and selecting it outlined every copy)."""
    xy = np.asarray(xy, np.float64)
    if len(xy) == 0:
        return 16.0
    lo, hi = np.percentile(xy, 2, axis=0), np.percentile(xy, 98, axis=0)
    return max(16.0, float(max(hi - lo)) / 16.0)


def cells(points, size):
    """The placements by map cell (x, y)"""
    xy = np.array([tuple(m.translation)[:2] for _, m in points], np.float64)
    keys = np.floor(xy / size).astype(np.int64)
    out = {}
    for (k, m), key in zip(points, map(tuple, keys)):
        out.setdefault(key, []).append((k, m))
    return out


_t = time.time()
WORLD_ARRAYS = [gather(i) for i in WORLD]
SPENT["meshes"] += time.time() - _t
if GN_POINTS:
    CELL = grid_size([tuple(m.translation)[:2] for ms in GN_POINTS.values() for m in ms])
else:
    CELL = grid_size(np.concatenate([g["pos"][g["tris"][::7, 0], :2] for g in WORLD_ARRAYS])
                     if WORLD_ARRAYS else [])
OBJECTS_WARN = 5000   # "objects" past this many placements: a warning (the viewport slows down)
if GN_POINTS and INSTANCES == "objects":
    _n = sum(len(v) for v in GN_POINTS.values())
    if _n > OBJECTS_WARN:
        print("GrayWolf: WARNING: PROPS = \"objects\" makes %d objects (one per placed prop): Blender's viewport "
              "and selection get slow past a few thousand. PROPS = \"fast\" keeps them in map cells." % _n)
    _by_cell = {}
    for mi in sorted(GN_POINTS):
        xy = np.floor(np.array([tuple(m.translation)[:2] for m in GN_POINTS[mi]], np.float64) / CELL).astype(np.int64)
        for m, nm, key in zip(GN_POINTS[mi], OBJ_NAMES[mi], map(tuple, xy)):
            _by_cell.setdefault(key, []).append((mi, nm, m))
    for (cx, cy), cp in sorted(_by_cell.items()):
        cc = collection("props_%d_%d" % (cx, cy), props_col)   # a collection per map cell
        for mi, nm, m in cp:
            me = mesh(mi)
            ob = bpy.data.objects.new(me.name, me)   # linked duplicates: one mesh per model
            ob["placement"] = nm
            ob.matrix_world = m
            cc.objects.link(ob)
        counts["instances"] += len(cp)
        counts["cells"] += 1
    GN_POINTS = {}   # done: no Geometry Nodes cells
elif GN_POINTS:
    pts_all = []
    for mi in sorted(GN_POINTS):
        k = proto(mi)["greyhoundx_index"]
        pts_all += [(k, m) for m in GN_POINTS[mi]]
    group = gn_group(models_col)
    cell_size = CELL
    by_cell = cells(pts_all, cell_size)
    for (cx, cy), cp in sorted(by_cell.items()):
        centre = Vector(((cx + 0.5) * cell_size, (cy + 0.5) * cell_size, 0.0))
        local = [Matrix.Translation(-centre) @ m for _, m in cp]
        me = bpy.data.meshes.new("props_%d_%d" % (cx, cy))
        me.vertices.add(len(cp))
        me.vertices.foreach_set("co", np.array([tuple(x.translation) for x in local], np.float32).ravel())
        me.attributes.new("model", "INT", "POINT").data.foreach_set("value", np.array([k for k, _ in cp], np.int32))
        if MATRIX_ATTR:
            me.attributes.new("placement", "FLOAT4X4", "POINT").data.foreach_set(
                "value", np.array([[x[r][c] for c in range(4) for r in range(4)] for x in local], np.float32).ravel())
        else:
            rs = [rotation_scale(x) for x in local]
            me.attributes.new("rotation", "FLOAT_VECTOR", "POINT").data.foreach_set(
                "vector", np.array([tuple(r) for r, _ in rs], np.float32).ravel())
            me.attributes.new("scale", "FLOAT_VECTOR", "POINT").data.foreach_set(
                "vector", np.array([tuple(s) for _, s in rs], np.float32).ravel())
        ob = bpy.data.objects.new(me.name, me)
        ob.location = centre
        md = ob.modifiers.new("placements", "NODES")
        md.node_group = group
        props_col.objects.link(ob)
        counts["instances"] += len(cp)
        counts["cells"] += 1


WORLD_CELL_TRIS = 5000   # a world cell bigger than the map cell holds at most this many triangles


def world_cells(arrays, size):
    """The world surfaces (one glTF mesh per material, each spanning the map) cut by map cell: each
    triangle goes to the cell of its centre, a cell is one object world_<x>_<y> (its origin the
    cell's centre) holding every material there; the materials stay shared. Sparse areas (distant
    scenery, a skybox's buildings) are cut coarser, as a quadtree: a square of 2^n cells stays one
    object world_<x>_<y>_x<2^n> while it holds at most WORLD_CELL_TRIS triangles (thousands of
    near-empty cells otherwise)."""
    arrays = [g for g in arrays if len(g["tris"])]
    if not arrays:
        return
    keys = [np.floor(g["pos"][g["tris"]].mean(axis=1)[:, :2] / size).astype(np.int64) for g in arrays]
    fine, inv, cnt = np.unique(np.concatenate(keys), axis=0, return_inverse=True, return_counts=True)
    inv = inv.ravel()
    top = int(max(fine.max(axis=0) - fine.min(axis=0))).bit_length()
    node = np.empty(len(fine), np.int64)
    nodes = []   # (level, x, y) in cells of that level
    stack = [(top, np.arange(len(fine)))]
    while stack:
        level, idx = stack.pop()
        k = np.floor_divide(fine[idx], 1 << level)
        uk, which = np.unique(k, axis=0, return_inverse=True)
        which = which.ravel()
        for j in range(len(uk)):
            members = idx[which == j]
            if level == 0 or cnt[members].sum() <= WORLD_CELL_TRIS:
                node[members] = len(nodes)
                nodes.append((level, int(uk[j][0]), int(uk[j][1])))
            else:
                stack.append((level - 1, members))
    by_cell = {}
    base = 0
    for g, kk in zip(arrays, keys):
        which = node[inv[base:base + len(kk)]]
        base += len(kk)
        for j in np.unique(which):
            by_cell.setdefault(int(j), []).append(subset(g, which == j))
    for j, parts in sorted(by_cell.items(), key=lambda e: nodes[e[0]]):
        level, cx, cy = nodes[j]
        side = size * (1 << level)
        t = time.time()
        origin = ((cx + 0.5) * side, (cy + 0.5) * side, 0.0)
        name = ("world_%d_%d" % (cx, cy) if level == 0 else
                "world_%d_%d_x%d" % (cx << level, cy << level, 1 << level))
        me = make_mesh(name, concat(parts), origin)
        SPENT["meshes"] += time.time() - t
        ob = bpy.data.objects.new(me.name, me)
        ob.location = origin
        ob["graywolf_world"] = True
        world_col.objects.link(ob)
        counts["world"] += 1


world_cells(WORLD_ARRAYS, CELL)
del WORLD_ARRAYS
# the model collections are only what the instances show; the lights (thousands on a big map) are
# hidden in the viewport and not selectable: their overlays (cones, lines to the ground) covered
# the map, a box selection took them too, and a viewport lit by them crawls. A render uses them.
for lc in bpy.context.view_layer.layer_collection.children[top.name].children:
    if lc.collection == models_col:
        lc.exclude = True
    elif lc.collection == lights_col and counts["lights"] > 0:
        lc.hide_viewport = True
lights_col.hide_select = True


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
        ob.hide_select = True   # around everything: any box selection would take it
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
    top.objects.link(ob)   # the sun stays in view (the other lights are hidden)
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


def setup_views():
    """Every 3D view framed on the map, its far clip beyond the map and its near clip at 10 cm
    (Blender's 1 cm to 1 km made coplanar surfaces z-fight in stripes seen from afar), the solid
    view showing the textures."""
    obs = [o for o in world_col.objects if o.type == "MESH" and o not in SKY_OBJECTS] + list(placed_col.objects)
    pts = [o.matrix_world @ Vector(c) for o in obs for c in o.bound_box]
    pts += [o.matrix_world.translation for o in props_col.objects]
    if not pts:
        return
    lo = Vector([min(p[i] for p in pts) for i in range(3)])
    hi = Vector([max(p[i] for p in pts) for i in range(3)])
    size = max((hi - lo).length, 10.0)
    for screen in bpy.data.screens:
        for area in screen.areas:
            for sp in area.spaces:
                if sp.type != "VIEW_3D":
                    continue
                sp.clip_start = 0.1
                sp.clip_end = max(sp.clip_end, 2.0 * size)
                sp.shading.color_type = "TEXTURE"
                if sp.region_3d is not None:
                    sp.region_3d.view_location = (lo + hi) / 2
                    sp.region_3d.view_distance = size * 0.6


setup_views()


# ---- map cells made editable on demand (GN instances are not selectable one by one, a world cell
# holds every surface of its few metres): Object > GrayWolf: Make selection editable (props as one
# object per placement, world as one object per piece), Object > GrayWolf: Realize placements,
# greyhoundx_realize(); the text block greyhoundx_realize.py registers them when the .blend opens
# (if scripts may run).
REALIZE_CODE = r'''
import bpy
import numpy as np
from mathutils import Euler, Matrix, Vector


def is_scatter(ob):
    return (ob is not None and ob.type == "MESH" and "model" in ob.data.attributes
            and any(m.type == "NODES" for m in ob.modifiers))


def scatter_models(ob):
    """{number: model object}: the children of the models collection a cell's node group instances"""
    for m in ob.modifiers:
        if m.type == "NODES" and m.node_group and "Collection Info" in m.node_group.nodes:
            c = m.node_group.nodes["Collection Info"].inputs["Collection"].default_value
            if c is None:
                return {}
            return {ch["greyhoundx_index"]: ch.objects[0] for ch in c.children
                    if "greyhoundx_index" in ch and len(ch.objects)}
    return {}


def values(a, prop, width):
    v = np.empty(len(a.data) * width, np.float32 if prop != "value" or a.data_type != "INT" else np.int32)
    a.data.foreach_get(prop, v)
    return v.reshape(-1, width) if width > 1 else v


def placements(ob):
    """the cell's placements in world space, and each one's model number"""
    A = ob.data.attributes
    model = values(A["model"], "value", 1)
    if "placement" in A:
        local = [Matrix(m.reshape(4, 4).T.tolist()) for m in values(A["placement"], "value", 16)]
    else:   # Blender before 4.2: position, Euler rotation, scale
        co = np.empty(len(ob.data.vertices) * 3, np.float32)
        ob.data.vertices.foreach_get("co", co)
        local = [Matrix.LocRotScale(Vector(p), Euler(r), Vector(s)) for p, r, s in
                 zip(co.reshape(-1, 3), values(A["rotation"], "vector", 3), values(A["scale"], "vector", 3))]
    return [ob.matrix_world @ m for m in local], model


def keep_only(ob, keep):
    """the cell's points reduced to the indices in keep (the object removed when none is left)"""
    old = ob.data
    if not keep:
        bpy.data.objects.remove(ob)
        bpy.data.meshes.remove(old)
        return
    k = np.array(keep)
    co = np.empty(len(old.vertices) * 3, np.float32)
    old.vertices.foreach_get("co", co)
    new = bpy.data.meshes.new(old.name)
    new.vertices.add(len(k))
    new.vertices.foreach_set("co", co.reshape(-1, 3)[k].ravel())
    for a in old.attributes:
        if a.name.startswith(".") or a.name == "position" or a.domain != "POINT":
            continue
        prop, width = {"INT": ("value", 1), "FLOAT_VECTOR": ("vector", 3), "FLOAT4X4": ("value", 16)}.get(a.data_type, (None, 0))
        if prop:
            v = values(a, prop, width)
            new.attributes.new(a.name, a.data_type, "POINT").data.foreach_set(prop, v[k].ravel())
    ob.data = new
    bpy.data.meshes.remove(old)


def region_test(box=None, planes=None):
    """inside(points: N x 3, world space) -> bool mask: an axis-aligned box (min corner, max corner),
    or a view's clipping planes (a, b, c, d), inside where a x + b y + c z + d > 0 (Alt+B's region)"""
    if planes is not None:
        P = np.array([tuple(p) for p in planes if Vector(tuple(p)[:3]).length > 0], np.float64).reshape(-1, 4)
        return lambda pts: (np.asarray(pts, np.float64).reshape(-1, 3) @ P[:, :3].T + P[:, 3] > 0).all(axis=1)
    lo, hi = np.asarray(box[0], np.float64), np.asarray(box[1], np.float64)
    return lambda pts: ((np.asarray(pts, np.float64).reshape(-1, 3) >= lo) &
                        (np.asarray(pts, np.float64).reshape(-1, 3) <= hi)).all(axis=1)


def _target(ob, collection):
    """where the objects made from ob go: the named collection (made at the scene's root), else ob's own"""
    if collection is None:
        return ob.users_collection[0] if ob.users_collection else bpy.context.scene.collection
    out = bpy.data.collections.get(collection)
    if out is None:
        out = bpy.data.collections.new(collection)
        bpy.context.scene.collection.children.link(out)
    return out


def _realize_take(ob, inside):
    models = scatter_models(ob)
    if not models:
        return models, [], [], []
    mats, model = placements(ob)
    ok = np.array([int(k) in models for k in model], bool)
    if inside is not None and len(mats):
        ok &= inside(np.array([tuple(m.translation) for m in mats], np.float64))
    return models, mats, model, [int(i) for i in np.nonzero(ok)[0]]


def greyhoundx_realize(objects=None, box=None, collection=None, inside=None):
    """Real objects for GrayWolf's Geometry Nodes placements: every placement of the cells given
    (default: the selected ones), or, with box = (min corner, max corner) in world space or inside
    (a region_test), those of every cell inside it. They leave the cell (no double), share their
    model's mesh and go to the cell's collection (or the collection named). Returns the new objects."""
    scene = bpy.context.scene
    if box is not None and inside is None:
        inside = region_test(box=box)
    if objects is None:
        objects = ([o for o in scene.objects if is_scatter(o)] if inside is not None else
                   [o for o in bpy.context.selected_objects if is_scatter(o)])
    made = []
    for ob in [o for o in objects if is_scatter(o)]:
        models, mats, model, take = _realize_take(ob, inside)
        out = _target(ob, collection)
        for i in take:
            src = models[int(model[i])]
            r = bpy.data.objects.new(src.name, src.data)
            r.matrix_world = mats[i]
            r["graywolf_placement"] = True
            out.objects.link(r)
            made.append(r)
        if take:
            chosen = set(take)
            keep_only(ob, [i for i in range(len(mats)) if i not in chosen])
    return made


# ---- world cells (world_<x>_<y>) cut into pieces: each connected part of a cell's surfaces
# (inside the region, by the centre of its faces) one object, the materials shared
def is_world(ob):
    return ob is not None and ob.type == "MESH" and bool(ob.get("graywolf_world"))


_ATTR = {"FLOAT": ("value", 1, np.float32), "INT": ("value", 1, np.int32), "BOOLEAN": ("value", 1, bool),
         "INT8": ("value", 1, np.int8), "FLOAT_VECTOR": ("vector", 3, np.float32),
         "FLOAT2": ("vector", 2, np.float32), "FLOAT_COLOR": ("color", 4, np.float32),
         "BYTE_COLOR": ("color_srgb", 4, np.float32), "INT32_2D": ("value", 2, np.int32),
         "QUATERNION": ("value", 4, np.float32), "FLOAT4X4": ("value", 16, np.float32)}
_SKIP = {"position", "material_index", "custom_normal"}   # custom normals: set from the corners'


def read_mesh(ob):
    """ob's mesh as arrays (faces, corners, attributes, corner normals, centres in world space)"""
    me = ob.data
    nv, nl, nf = len(me.vertices), len(me.loops), len(me.polygons)
    d = {"me": me, "co": np.empty(nv * 3, np.float32), "vi": np.empty(nl, np.int32),
         "ls": np.empty(nf, np.int32), "lt": np.empty(nf, np.int32), "mi": np.empty(nf, np.int32)}
    me.vertices.foreach_get("co", d["co"])
    d["co"] = d["co"].reshape(-1, 3)
    me.loops.foreach_get("vertex_index", d["vi"])
    me.polygons.foreach_get("loop_start", d["ls"])
    me.polygons.foreach_get("loop_total", d["lt"])
    me.polygons.foreach_get("material_index", d["mi"])
    d["attrs"] = []
    for a in me.attributes:
        if a.name.startswith(".") or a.name in _SKIP or a.domain not in ("POINT", "CORNER", "FACE"):
            continue
        spec = _ATTR.get(a.data_type)
        if spec is None:
            continue
        prop, w, dt = spec
        v = np.empty(len(a.data) * w, dt)
        try:
            a.data.foreach_get(prop, v)
        except (AttributeError, TypeError, RuntimeError):
            if prop != "color_srgb":
                continue
            prop = "color"
            a.data.foreach_get(prop, v)
        d["attrs"].append((a.name, a.data_type, a.domain, prop, v.reshape(-1, w) if w > 1 else v))
    cn = np.empty(nl * 3, np.float32)
    if hasattr(me, "corner_normals"):   # 4.1+
        me.corner_normals.foreach_get("vector", cn)
    else:
        me.calc_normals_split()
        me.loops.foreach_get("normal", cn)
    d["cn"] = cn.reshape(-1, 3)
    d["custom"] = bool(me.has_custom_normals)
    mw = np.array(ob.matrix_world, np.float64)
    loops, starts = _corners(d, np.arange(nf))
    sums = np.add.reduceat(d["co"][d["vi"][loops]].astype(np.float64), starts) if nf else np.zeros((0, 3))
    d["world"] = (sums / np.maximum(d["lt"], 1)[:, None]) @ mw[:3, :3].T + mw[:3, 3]   # face centres
    return d


def _corners(d, faces):
    lt = d["lt"][faces]
    starts = np.cumsum(lt) - lt
    return np.repeat(d["ls"][faces] - starts, lt) + np.arange(int(lt.sum())), starts


def loose_parts(d, faces):
    """each face's part number (faces sharing a vertex are one part), for the faces given"""
    loops, starts = _corners(d, faces)
    a = np.repeat(d["vi"][d["ls"][faces]], d["lt"][faces])
    b = d["vi"][loops]
    parent = np.arange(len(d["co"]))
    while True:
        pa, pb = parent[a], parent[b]
        if (pa == pb).all():
            break
        low = np.minimum(pa, pb)
        np.minimum.at(parent, pa, low)
        np.minimum.at(parent, pb, low)
        while True:
            pp = parent[parent]
            if (pp == parent).all():
                break
            parent = pp
    _, part = np.unique(parent[d["vi"][d["ls"][faces]]], return_inverse=True)
    return part.ravel()


def build_part(d, faces, name, offset):
    """a new mesh of the faces given, its vertices moved by -offset"""
    loops, starts = _corners(d, faces)
    used, inv = np.unique(d["vi"][loops], return_inverse=True)
    me = bpy.data.meshes.new(name)
    me.vertices.add(len(used))
    me.vertices.foreach_set("co", (d["co"][used] - np.asarray(offset, np.float32)).astype(np.float32).ravel())
    me.loops.add(len(loops))
    me.loops.foreach_set("vertex_index", inv.ravel().astype(np.int32))
    me.polygons.add(len(faces))
    me.polygons.foreach_set("loop_start", starts.astype(np.int32))
    um, slot = np.unique(d["mi"][faces], return_inverse=True)
    src = d["me"].materials
    for m in um:
        me.materials.append(src[int(m)] if int(m) < len(src) else None)
    me.polygons.foreach_set("material_index", slot.ravel().astype(np.int32))
    for an, dtype, domain, prop, v in d["attrs"]:
        sel = {"POINT": used, "CORNER": loops, "FACE": faces}[domain]
        me.attributes.new(an, dtype, domain).data.foreach_set(prop, v[sel].ravel())
    me.update(calc_edges=True)
    if d["custom"]:
        if hasattr(me, "use_auto_smooth"):   # before 4.1
            me.use_auto_smooth = True
        me.normals_split_custom_set(d["cn"][loops])
    return me


SMALL_PART = 16   # faces: a part with fewer joins the others of its material


def split_world(ob, inside=None, collection=None, plan=False):
    """ob's faces inside the region (all without one) as one object per connected part (the small ones
    one per material), named after
    its material, its origin the part's centre; the rest stays in ob (removed when empty). With
    plan, only counts the parts. Returns the new objects (or the count)."""
    d = read_mesh(ob)
    nf = len(d["ls"])
    faces = np.arange(nf) if inside is None else np.nonzero(inside(d["world"]))[0]
    if len(faces) == 0:
        return 0 if plan else []
    part = loose_parts(d, faces)
    # the small parts (decals, trims: a few faces each, hundreds per cell) one object per material
    small = np.bincount(part)[part] < SMALL_PART
    if small.any():
        part = np.where(small, part.max() + 1 + d["mi"][faces], part)
        _, part = np.unique(part, return_inverse=True)
        part = part.ravel()
    if plan:
        return int(part.max()) + 1
    out = _target(ob, collection)
    made = []
    order = np.argsort(part, kind="stable")
    bounds = np.r_[0, np.nonzero(np.diff(part[order]))[0] + 1, len(order)]
    mw = ob.matrix_world
    for s, e in zip(bounds[:-1], bounds[1:]):
        f = faces[order[s:e]]
        loops, _ = _corners(d, f)
        pts = d["co"][d["vi"][loops]]
        centre = (pts.min(axis=0) + pts.max(axis=0)) / 2
        mat = d["me"].materials[int(d["mi"][f[0]])] if len(d["me"].materials) else None
        me = build_part(d, f, mat.name if mat else ob.name, centre)
        r = bpy.data.objects.new(me.name, me)
        r.matrix_world = mw @ Matrix.Translation(Vector(centre.tolist()))
        r["graywolf_piece"] = ob.name
        out.objects.link(r)
        made.append(r)
    rest = np.setdiff1d(np.arange(nf), faces)
    old = ob.data
    if len(rest) == 0:
        bpy.data.objects.remove(ob)
    else:
        ob.data = build_part(d, rest, old.name, (0.0, 0.0, 0.0))
    if old.users == 0:
        bpy.data.meshes.remove(old)
    return made


def make_editable(cells, inside=None, limit=5000, collection=None):
    """GrayWolf's map cells made editable: the props (props_<x>_<y>) as one object per placement,
    the world (world_<x>_<y>) as one object per connected piece, all of each cell or, with inside
    (a region_test), only what is in that region. Refused (ValueError) past limit new objects.
    Returns the new objects."""
    props = [o for o in cells if is_scatter(o)]
    world = [o for o in cells if is_world(o)]
    n = sum(len(_realize_take(o, inside)[3]) for o in props)
    n += sum(split_world(o, inside, plan=True) for o in world)
    if limit and n > limit:
        raise ValueError("%d objects would be made (limit %d): choose a smaller region" % (n, limit))
    made = greyhoundx_realize(objects=props, inside=inside, collection=collection) if props else []
    for o in world:
        made += split_world(o, inside, collection)
    return made


def view_clip_planes(context):
    """the Alt+B clipping region's planes of the 3D view (the one the operator runs in, else any)"""
    views = [getattr(context, "region_data", None)]
    for area in context.screen.areas if context.screen else []:
        for sp in area.spaces:
            if sp.type == "VIEW_3D":
                views.append(sp.region_3d)
    for rv in views:
        if rv is not None and getattr(rv, "use_clip_planes", False):
            return [tuple(p) for p in rv.clip_planes]
    return None


class GRAYWOLF_OT_make_editable(bpy.types.Operator):
    """Make the selected map cells editable: the props in them become one object per placed model
    (sharing its mesh), the world surfaces one object per connected piece. With an Alt+B clipping
    region, only what lies in that region, in any cell"""
    bl_idname = "graywolf.make_editable"
    bl_label = "GrayWolf: Make selection editable"
    bl_options = {"REGISTER", "UNDO"}
    region: bpy.props.EnumProperty(name="Region", items=[
        ("AUTO", "Automatic", "The Alt+B clipping region when the view has one, else the selected cells"),
        ("SELECTED", "Selected cells", "Everything in the selected cells (props_*, world_*)"),
        ("CLIP", "Clipping region (Alt+B)", "What lies inside the view's Alt+B region, in any cell"),
        ("BOUNDS", "Inside the selection's bounds", "What lies inside the bounds of the selected objects that are not cells (a cube)")])
    limit: bpy.props.IntProperty(name="Max new objects", default=5000, min=0,
                                 description="Refuse past this many new objects (0: no limit)")

    def execute(self, context):
        region = self.region
        planes = view_clip_planes(context) if region in ("AUTO", "CLIP") else None
        if region == "CLIP" and planes is None:
            self.report({"WARNING"}, "no clipping region in this view: Alt+B, then drag a box")
            return {"CANCELLED"}
        inside, cells = None, None
        if planes is not None:
            inside = region_test(planes=planes)
            cells = [o for o in context.visible_objects if is_scatter(o) or is_world(o)]
        elif region == "BOUNDS":
            boxes = [o for o in context.selected_objects if not (is_scatter(o) or is_world(o))]
            if not boxes:
                self.report({"WARNING"}, "select an object whose bounds enclose the region (a cube)")
                return {"CANCELLED"}
            pts = [o.matrix_world @ Vector(c) for o in boxes for c in o.bound_box]
            inside = region_test(box=([min(p[i] for p in pts) for i in range(3)], [max(p[i] for p in pts) for i in range(3)]))
            cells = [o for o in context.visible_objects if is_scatter(o) or is_world(o)]
        else:
            cells = [o for o in context.selected_objects if is_scatter(o) or is_world(o)]
            if not cells:
                self.report({"WARNING"}, "select map cells (props_*, world_*) first, e.g. with a box selection")
                return {"CANCELLED"}
        try:
            made = make_editable(cells, inside, self.limit)
        except ValueError as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        for o in context.selected_objects:
            o.select_set(False)
        for o in made:
            o.select_set(True)
        if made:
            context.view_layer.objects.active = made[0]
        self.report({"INFO"}, "%d objects made editable" % len(made))
        return {"FINISHED"}


class GREYHOUNDX_OT_realize_placements(bpy.types.Operator):
    """Turn GrayWolf's Geometry Nodes placements into real, selectable objects: all those of the
    selected cells (props_<x>_<y>), or those inside the bounds of the other selected objects (a box)"""
    bl_idname = "greyhoundx.realize_placements"
    bl_label = "GrayWolf: Realize placements"
    bl_options = {"REGISTER", "UNDO"}
    mode: bpy.props.EnumProperty(items=[
        ("SELECTED", "Selected cells", "Every placement of the selected cells"),
        ("BOX", "Inside the selection's bounds", "Placements of any cell inside the bounds of the selected objects that are not cells")])

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
    self.layout.separator()
    self.layout.operator(GRAYWOLF_OT_make_editable.bl_idname, text="GrayWolf: Make selection editable")
    self.layout.operator_menu_enum(GREYHOUNDX_OT_realize_placements.bl_idname, "mode", text="GrayWolf: Realize placements")


def register():
    for cls in (GRAYWOLF_OT_make_editable, GREYHOUNDX_OT_realize_placements):
        try:
            bpy.utils.unregister_class(getattr(bpy.types, cls.__name__))
        except (AttributeError, RuntimeError):
            pass
        bpy.utils.register_class(cls)
    old = bpy.app.driver_namespace.get("greyhoundx_menu")
    if old:
        try:
            bpy.types.VIEW3D_MT_object.remove(old)
        except Exception:
            pass
    bpy.types.VIEW3D_MT_object.append(_menu)
    bpy.app.driver_namespace["greyhoundx_menu"] = _menu
    bpy.app.driver_namespace["greyhoundx_realize"] = greyhoundx_realize
    bpy.app.driver_namespace["graywolf_make_editable"] = make_editable


register()
'''
_txt = bpy.data.texts.get("greyhoundx_realize.py") or bpy.data.texts.new("greyhoundx_realize.py")
_txt.from_string(REALIZE_CODE)
_txt.use_module = True
_ns = {"__name__": "greyhoundx_realize"}
exec(compile(REALIZE_CODE, "greyhoundx_realize.py", "exec"), _ns)
greyhoundx_realize, make_editable, region_test = _ns["greyhoundx_realize"], _ns["make_editable"], _ns["region_test"]

HOWTO = """GrayWolf map - how to select a part of the map
1. Click picks one object; a box selection (drag, or B) picks only the map cells under it:
   world_<x>_<y> (walls, floors) and props_<x>_<y> (placed models), each a few metres wide.
2. Alt+B then drag a box: the view shows only that region (Alt+B again to show everything).
3. Numpad / (View > Local View): only the selected objects, / again to come back.
4. Object > GrayWolf: Make selection editable (or F3, "Make selection editable"): the props in
   the selected cells become one object per placed model, the world one object per piece, so each
   can be picked alone. With an Alt+B region, only what is inside it. Ctrl+Z undoes it.
5. The lights are hidden: the eye of the 'lights' collection shows them.
6. Python: greyhoundx_realize(box=((x0, y0, z0), (x1, y1, z1))) realizes the props in a box."""
_how = bpy.data.texts.get("GrayWolf map - how to") or bpy.data.texts.new("GrayWolf map - how to")
_how.from_string(HOWTO + "\n")

print("Greyhoundx: imported %s in %.1fs (meshes %.1fs, materials %.1fs): %d objects, %d world cells, %d instances "
      "of %d models in %d cells (%.0f m), %d lights (hidden in the viewport), %d empties, %d materials" % (
          GLB, time.time() - T0, SPENT["meshes"] - SPENT["materials"], SPENT["materials"], counts["objects"],
          counts["world"], counts["instances"], len(PROTOS), counts["cells"], CELL, counts["lights"],
          counts["empties"], len(MATERIALS_BY_INDEX)))
print(HOWTO)


def _report_howto():
    """the how-to's first line in the status bar and the Info editor (a script has no report of its own)"""
    class GRAYWOLF_OT_howto_report(bpy.types.Operator):
        bl_idname = "graywolf.howto_report"
        bl_label = "GrayWolf map how-to"
        bl_options = {"INTERNAL"}

        def execute(self, context):
            self.report({"INFO"}, "GrayWolf: map imported. Box select picks map cells; Alt+B clips a region; "
                                  "Object > GrayWolf: Make selection editable; Text Editor: 'GrayWolf map - how to'")
            return {"FINISHED"}
    try:
        bpy.utils.register_class(GRAYWOLF_OT_howto_report)
        bpy.ops.graywolf.howto_report()
    except Exception:
        pass


_report_howto()
