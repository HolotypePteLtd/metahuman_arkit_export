"""Headless UE Python: dump CR_MetaHuman_HeadMovement_IK_Proc RigVM graph to JSON.

Loads the project ControlRig asset, walks its RigVMGraph, and emits every node
(unit type, script struct path), every pin/literal value, every link, and the
director/graph metadata needed to reimplement the graph outside UE.
Run: UnrealEditor-Cmd <uproject> -ExecutePythonScript=... -unattended -nop4
"""
import json
import unreal

OUT = "/mnt/sdd_1tb/mh_arkit_export/scratch/cr_headmovement_dump.json"
ASSET = "/Game/MetaHumans/Common/Face/CR_MetaHuman_HeadMovement_IK_Proc"


def pin_value(pin):
    """Best-effort literal extraction from an RigVMPin."""
    if pin is None:
        return None
    d = {"name": pin.get_name(), "type": str(pin.get_type())}
    try:
        d["display_name"] = pin.get_display_name()
    except Exception:
        pass
    try:
        default = pin.get_default_value()
        if default not in (None, ""):
            d["default_value"] = default
    except Exception:
        pass
    sub = []
    try:
        for sp in pin.get_sub_pins():
            sub.append(pin_value(sp))
    except Exception:
        pass
    if sub:
        d["pins"] = sub
    return d


def main():
    rig = unreal.load_asset(ASSET)
    if rig is None:
        raise SystemExit("could not load %s" % ASSET)

    # Find the RigVMController / graph. ControlRig exposes get_controller() in
    # editor builds; older APIs: unreal.ControlRigBlueprint.get_controller().
    ctrl = None
    if hasattr(rig, "get_controller"):
        ctrl = rig.get_controller()
    if ctrl is None and hasattr(rig, "controller"):
        ctrl = rig.controller

    graph = None
    if ctrl is not None and hasattr(ctrl, "get_graph"):
        graph = ctrl.get_graph()

    out = {"asset": ASSET, "nodes": [], "links": [], "variables": []}

    if graph is not None:
        for node in graph.get_nodes():
            nd = {
                "name": node.get_name(),
                "node_class": node.__class__.__name__,
            }
            try:
                nd["script_struct_path"] = str(node.find_pin(
                    "ExecuteContext").get_type())
            except Exception:
                pass
            # Unit node: script struct = the RigVM function
            if hasattr(node, "get_script_struct"):
                try:
                    nd["script_struct"] = str(node.get_script_struct().get_name())
                    nd["script_struct_path"] = str(
                        node.get_script_struct().get_path_name())
                except Exception:
                    pass
            if hasattr(node, "get_node_title"):
                try:
                    nd["title"] = str(node.get_node_title())
                except Exception:
                    pass
            pins = []
            try:
                for p in node.get_pins():
                    pins.append(pin_value(p))
            except Exception:
                pass
            nd["pins"] = pins
            out["nodes"].append(nd)

        for link in graph.get_links():
            out["links"].append({
                "from": "%s.%s" % (link.get_source_pin().get_node().get_name(),
                                   link.get_source_pin().get_name()),
                "to": "%s.%s" % (link.get_target_pin().get_node().get_name(),
                                 link.get_target_pin().get_name()),
            })

    # Blueprint variables (function inputs / public data)
    try:
        for v in unreal.BlueprintFunctionLibrary.get_blueprint_variables(rig):
            out["variables"].append(str(v))
    except Exception:
        pass

    with open(OUT, "w") as f:
        json.dump(out, f, indent=1)
    print("wrote %s (%d nodes, %d links)" %
          (OUT, len(out["nodes"]), len(out["links"])))


main()
