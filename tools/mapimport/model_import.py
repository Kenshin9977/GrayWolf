# The body of <model>_import.py, the Blender script GrayWolf writes beside every Cast or glTF model:
# ModelImportScript.cpp writes the parameters, then everything after the body marker below
# (embedded by embed.py, which puts materials.py, shared with the map import script, in place of
# the "# ---- materials.py" line). To test by hand, export a model and edit the script written beside it.
MODELS = ["model_LOD0.cast", "model_LOD0.glb", "model_LOD0.gltf"]
MATERIALS = "model_LOD0.materials.json"
SCALE = 0.01
# ---- body
import bpy, os, json, struct, time, re
import numpy as np
from mathutils import Matrix, Quaternion, Vector

# ---- materials.py


DIR = script_dir(MODELS)
T0 = time.time()
MODEL = next(n for n in MODELS if os.path.isfile(os.path.join(DIR, n)))
NAME = os.path.splitext(MODEL)[0]

# ---- materials: the sidecar's, by name (the model file's own material is kept for a name it lacks)
SIDECAR = {}
try:
    with open(os.path.join(DIR, MATERIALS), encoding="utf-8") as f:
        SIDECAR = {m["name"]: m for m in json.load(f).get("materials", [])}
except Exception as e:
    print("GrayWolf: %s: %s (the model keeps its file's materials)" % (MATERIALS, e))
BUILT = {}


def model_material(name):
    """The material built from the sidecar for NAME (a ".001" Blender added left out), else None."""
    base = re.sub(r"\.\d{3}$", "", name)
    if base not in BUILT:
        d = SIDECAR.get(base)
        BUILT[base] = None if d is None else invisible_material(base) if normal_only(d) else sidecar_material(base, d, None)
    return BUILT[base]


# ---- Cast (github.com/dtzxporter/cast), read here: no add-on needed
CAST_TYPES = {0x62: (np.uint8, 1), 0x68: (np.uint16, 1), 0x69: (np.uint32, 1), 0x6C: (np.uint64, 1),
              0x66: (np.float32, 1), 0x64: (np.float64, 1), 0x7632: (np.float32, 2), 0x7633: (np.float32, 3),
              0x7634: (np.float32, 4)}
ID = {"model": 0x6C646F6D, "mesh": 0x6873656D, "blend": 0x68736C62, "skeleton": 0x6C656B73, "bone": 0x656E6F62,
      "material": 0x6C74616D}


class CastNode:
    def __init__(self, ident, hash_):
        self.id, self.hash, self.props, self.children = ident, hash_, {}, []

    def of(self, kind):
        return [c for c in self.children if c.id == ID[kind]]

    def one(self, name, default=None):
        v = self.props.get(name)
        return default if v is None else v if isinstance(v, str) else v[0]


def read_node(data, off):
    ident, size, hash_, nprops, nchildren = struct.unpack_from("<IIQII", data, off)
    node = CastNode(ident, hash_)
    p = off + 24
    for _ in range(nprops):
        pid, nlen, count = struct.unpack_from("<HHI", data, p)
        p += 8
        name = data[p:p + nlen].decode("utf-8", "replace")
        p += nlen
        if pid == 0x73:   # strings, each ended by a zero
            vals = []
            for _ in range(count):
                e = data.index(b"\0", p)
                vals.append(data[p:e].decode("utf-8", "replace"))
                p = e + 1
            node.props[name] = vals[0] if count == 1 else vals
            continue
        dt, w = CAST_TYPES[pid]
        arr = np.frombuffer(data, dt, count * w, p)
        p += arr.nbytes
        node.props[name] = arr.reshape(-1, w) if w > 1 else arr
    for _ in range(nchildren):
        child, p = read_node(data, p)
        node.children.append(child)
    return node, off + size


def read_cast(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, _, roots, _ = struct.unpack_from("<4I", data, 0)
    if magic != 0x74736163:
        raise ValueError("%s is not a Cast file" % path)
    off, nodes = 16, []
    for _ in range(roots):
        n, off = read_node(data, off)
        nodes.append(n)
    return nodes


def build_armature(skel, col):
    bones = skel.of("bone") if skel else []
    if not bones:
        return None, []
    arm = bpy.data.armatures.new(NAME)
    ob = bpy.data.objects.new(NAME, arm)
    col.objects.link(ob)
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.mode_set(mode="EDIT")
    names, made = [], []
    for b in bones:
        eb = arm.edit_bones.new(b.one("n", "bone"))
        eb.tail = (0.0, 0.03, 0.0)   # the bone along its joint's y axis
        q = b.props.get("wr")
        t = b.props.get("wp")
        m = Matrix.Translation(Vector(t[0]) * SCALE) if t is not None else Matrix.Identity(4)
        if q is not None:
            x, y, z, w = (float(v) for v in q[0])
            m = m @ Quaternion((w, x, y, z)).to_matrix().to_4x4()
        eb.matrix = m
        made.append(eb)
        names.append(eb.name)
    for b, eb in zip(bones, made):
        parent = int(b.one("p", 0xFFFFFFFF))   # an unsigned 32-bit, -1 for a root
        if 0 <= parent < len(made):
            eb.parent = made[parent]
    bpy.ops.object.mode_set(mode="OBJECT")
    return ob, names


def cast_mesh(node, name, mat):
    P = np.asarray(node.props["vp"], np.float32) * SCALE
    N = node.props.get("vn")
    tris = np.asarray(node.props["f"], np.int64).reshape(-1, 3)
    # the winding Cast stores is CoD's: faces turned so their normal follows the vertices'
    if N is not None and len(tris):
        fn = np.cross(P[tris[:, 1]] - P[tris[:, 0]], P[tris[:, 2]] - P[tris[:, 0]])
        if (np.einsum("ij,ij->i", fn, N[tris].sum(axis=1)) < 0).sum() * 2 > len(tris):
            tris = tris[:, ::-1]
    keep = (tris[:, 0] != tris[:, 1]) & (tris[:, 1] != tris[:, 2]) & (tris[:, 0] != tris[:, 2])
    idx = np.ascontiguousarray(tris[keep]).ravel().astype(np.int32)
    me = bpy.data.meshes.new(name)
    me.vertices.add(len(P))
    me.vertices.foreach_set("co", P.ravel())
    me.loops.add(len(idx))
    me.loops.foreach_set("vertex_index", idx)
    me.polygons.add(len(idx) // 3)
    me.polygons.foreach_set("loop_start", np.arange(0, len(idx), 3, dtype=np.int32))
    for layer in range(int(node.one("ul", 1))):
        u = node.props.get("u%d" % layer)
        if u is None:
            continue
        uvs = np.array(u, np.float32)[idx]
        uvs[:, 1] = 1.0 - uvs[:, 1]
        me.uv_layers.new(name="UVMap" if layer == 0 else "UVMap%d" % layer).uv.foreach_set("vector", uvs.ravel())
    c = node.props.get("c0")
    if c is not None:
        c = np.asarray(c, np.uint32).view(np.uint8).reshape(-1, 4)
        if (c != 255).any():
            me.color_attributes.new("Col", "BYTE_COLOR", "POINT").data.foreach_set("color_srgb", (c.astype(np.float32) / 255.0).ravel())
    if mat is not None:
        me.materials.append(mat)
    me.validate(clean_customdata=False)
    me.update()
    if N is not None:
        if hasattr(me, "use_auto_smooth"):   # before 4.1, custom normals show only with it
            me.use_auto_smooth = True
        me.normals_split_custom_set_from_vertices(np.asarray(N, np.float32))
    try:
        me.polygons.foreach_set("use_smooth", np.ones(len(me.polygons), bool))
    except (AttributeError, TypeError):
        pass
    return me


def skin(ob, node, arm, names):
    mi = int(node.one("mi", 0))
    if arm is None or mi == 0 or "wb" not in node.props:
        return
    n = len(node.props["vp"])
    b = np.asarray(node.props["wb"], np.int64)[:n * mi]
    w = np.asarray(node.props["wv"], np.float32)[:n * mi]
    v = np.repeat(np.arange(n), mi)
    keep = (w > 0) & (b < len(names))
    v, b, w = v[keep], b[keep], w[keep]
    order = np.lexsort((w, b))
    v, b, w = v[order], b[order], w[order]
    # one call per (bone, weight): rigid parts are whole groups at 1.0
    cut = np.flatnonzero((np.diff(b) != 0) | (np.diff(w) != 0)) + 1
    groups = {}
    for s, e in zip(np.concatenate(([0], cut)), np.concatenate((cut, [len(b)]))):
        if s == e:
            continue
        bone = int(b[s])
        if bone not in groups:
            groups[bone] = ob.vertex_groups.new(name=names[bone])
        groups[bone].add(v[s:e].tolist(), float(w[s]), "REPLACE")
    ob.parent = arm
    ob.modifiers.new("Armature", "ARMATURE").object = arm


def import_cast(path, col):
    counts = {"meshes": 0, "bones": 0, "shapes": 0}
    for root in read_cast(path):
        for model in root.of("model"):
            mats = {}
            for m in model.of("material"):
                name = m.one("n", "material")
                mats[m.hash] = model_material(name) or bpy.data.materials.new(name)
            skel = model.of("skeleton")
            arm, names = build_armature(skel[0] if skel else None, col)
            counts["bones"] += len(names)
            by_hash = {}
            for node in model.of("mesh"):
                mat = mats.get(int(node.one("m", 0)))
                me = cast_mesh(node, mat.name if mat else NAME, mat)
                ob = bpy.data.objects.new(me.name, me)
                col.objects.link(ob)
                skin(ob, node, arm, names)
                by_hash[node.hash] = (ob, node)
                counts["meshes"] += 1
            for bs in model.of("blend"):   # shape keys: the target's positions of the listed vertices
                base = by_hash.get(int(bs.one("b", 0)))
                if base is None or "vi" not in bs.props:
                    continue
                ob, node = base
                if ob.data.shape_keys is None:
                    ob.shape_key_add(name="Basis")
                co = np.asarray(node.props["vp"], np.float32) * SCALE
                co[np.asarray(bs.props["vi"], np.int64)] = np.asarray(bs.props["vp"], np.float32) * SCALE
                ob.shape_key_add(name=bs.one("n", "shape"), from_mix=False).data.foreach_set("co", co.ravel())
                counts["shapes"] += 1
    return counts


def import_gltf(path, col):
    """Blender's own glTF importer, then each material replaced by the sidecar's."""
    before_obj, before_mat, before_img = set(bpy.data.objects), set(bpy.data.materials), set(bpy.data.images)
    bpy.ops.import_scene.gltf(filepath=path)
    new = [o for o in bpy.data.objects if o not in before_obj]
    # the importer's bone display shape (an icosphere) is no part of the model: left out of the scene
    shapes = {pb.custom_shape for o in new if o.type == "ARMATURE" for pb in o.pose.bones if pb.custom_shape}
    for o in new:
        for c in list(o.users_collection):
            c.objects.unlink(o)
        if o in shapes:
            continue
        col.objects.link(o)
        for slot in o.material_slots:
            if slot.material is not None:
                m = model_material(slot.material.name)
                if m is not None:
                    slot.material = m
    # the importer's materials and images replaced: removed (its DDS images it could not read too)
    for m in [m for m in bpy.data.materials if m not in before_mat and m.users == 0]:
        bpy.data.materials.remove(m)
    for i in [i for i in bpy.data.images if i not in before_img and i.users == 0]:
        bpy.data.images.remove(i)
    new = [o for o in new if o not in shapes]
    return {"meshes": sum(o.type == "MESH" for o in new),
            "bones": sum(len(o.data.bones) for o in new if o.type == "ARMATURE"), "shapes": 0}


if bpy.context.object is not None and bpy.context.object.mode != "OBJECT":
    bpy.ops.object.mode_set(mode="OBJECT")
COL = bpy.data.collections.new(NAME)
bpy.context.scene.collection.children.link(COL)
COUNTS = (import_cast if MODEL.lower().endswith(".cast") else import_gltf)(os.path.join(DIR, MODEL), COL)

made = [m for m in BUILT.values() if m is not None]
wired = sum(any(n.type == "TEX_IMAGE" and n.image for n in m.node_tree.nodes) for m in made)
print("GrayWolf: imported %s in %.1fs: %d meshes, %d bones, %d shape keys, %d materials from %s (%d with textures)" % (
    MODEL, time.time() - T0, COUNTS["meshes"], COUNTS["bones"], COUNTS["shapes"], len(made), MATERIALS, wired))
if UNREAD:
    print("GrayWolf: %d images Blender could not read (export the images as PNG, Settings > Image format, "
          "for the textures these materials leave out):" % len(UNREAD))
    for p, why in sorted(UNREAD.items()):
        print("    %s: %s" % (p, why))
