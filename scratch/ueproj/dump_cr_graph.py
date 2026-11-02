"""Headless UE Python v3: dump CR RigVM graph with pin literals + errors."""
import json
import traceback
import unreal

OUT = "/mnt/sdd_1tb/mh_arkit_export/scratch/cr_headmovement_dump.json"
ASSET = "/Game/MetaHumans/Common/Face/CR_MetaHuman_HeadMovement_IK_Proc"

errors = []


def dump_pin_recursive(ctrl, node_name, pin, path_prefix, depth, out_list):
    path = pin.get_name()
    full = path_prefix + "." + path if path_prefix else path
    rec = {"name": path}
    try:
        dv = ctrl.get_pin_default_value("%s.%s" % (node_name, full))
        if dv not in (None, ""):
            rec["default_value"] = dv
    except Exception as e:
        errors.append("pin_default(%s.%s): %s" % (node_name, full, e))
    out_list.append(rec)
    if depth < 6:
        try:
            for sp in pin.get_sub_pins():
                dump_pin_recursive(ctrl, node_name, sp, full, depth + 1, out_list[-1].setdefault("pins", []))
        except Exception as e:
            errors.append("subpins(%s.%s): %s" % (node_name, full, e))


def main():
    rig = unreal.load_asset(ASSET)
    if rig is None:
        raise SystemExit("could not load %s" % ASSET)

    ctrl = rig.get_controller() if hasattr(rig, "get_controller") else None
    if ctrl is None and hasattr(rig, "controller"):
        ctrl = rig.controller
    graph = ctrl.get_graph() if ctrl and hasattr(ctrl, "get_graph") else None

    out = {"asset": ASSET, "nodes": [], "links": [], "variables": [], "errors": errors}

    if graph is None:
        errors.append("no graph via controller; rig type %s" % type(rig))
    else:
        for node in graph.get_nodes():
            name = node.get_name()
            nd = {"name": name, "node_class": node.__class__.__name__}
            if hasattr(node, "get_script_struct"):
                try:
                    nd["script_struct"] = str(node.get_script_struct().get_path_name())
                except Exception as e:
                    errors.append("script_struct(%s): %s" % (name, e))
            pins = []
            try:
                for p in node.get_pins():
                    dump_pin_recursive(ctrl, name, p, "", 0, pins)
            except Exception as e:
                errors.append("get_pins(%s): %s %s" % (name, e, traceback.format_exc()[-300:]))
            nd["pins"] = pins
            out["nodes"].append(nd)

        for link in graph.get_links():
            try:
                out["links"].append({
                    "from": "%s.%s" % (link.get_source_pin().get_node().get_name(),
                                       link.get_source_pin().get_name()),
                    "to": "%s.%s" % (link.get_target_pin().get_node().get_name(),
                                     link.get_target_pin().get_name()),
                })
            except Exception as e:
                errors.append("link: %s" % e)

    try:
        for v in rig.get_editor_property("member_vars"):
            out["variables"].append(str(v))
    except Exception as e:
        errors.append("member_vars: %s" % e)

    with open(OUT, "w") as f:
        json.dump(out, f, indent=1)
    print("wrote %s (%d nodes, %d links, %d errors)" %
          (OUT, len(out["nodes"]), len(out["links"]), len(errors)))


main()
