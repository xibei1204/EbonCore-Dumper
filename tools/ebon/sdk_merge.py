"""Merge verified same-build reflection scenes, retaining disagreements and evidence provenance."""
from copy import deepcopy
import json
from pathlib import Path
import shutil

from .sdk_codegen import CodegenError, _validate_dump
from .reflection_evidence import replay_alignments, replay_property_targets


def layout_signature(owner):
    number=lambda v:int(v,0) if isinstance(v,str) else v
    keys=('kind','name','size','parameter_size','super','flags')
    fields=sorted((tuple(number(f.get(k)) if k=='flags' else f.get(k) for k in ('name','type','offset','element_size','array_dim','flags'))
                   for f in owner['fields']),key=lambda f:f[0])
    return tuple(number(owner.get(k)) if k=='flags' else owner.get(k) for k in keys),fields,owner.get('enum_values',[])


def _enrich(left,right,path,changes,conflicts):
    for key in ('target','bool_mask','alignment','role'):
        value=right.get(key)
        if value is not None and value!='':
            if left.get(key) in (None,''):
                left[key]=deepcopy(value);changes.append(path+'.'+key)
            elif left[key]!=value:conflicts.append({'path':path,'key':key,'values':[left[key],value]})
    for key in ('detail','inner','key','value','underlying'):
        value=right.get(key)
        if isinstance(value,dict):
            if not isinstance(left.get(key),dict):left[key]={}
            if key!='detail' and any(left[key].get(k) is not None and value.get(k) is not None and left[key][k]!=value[k] for k in ('type','size')):
                conflicts.append({'path':path,'key':key,'reason':'nested property layout changed'});continue
            for k in ('type','size'):
                if k not in left[key] and k in value:left[key][k]=value[k]
            _enrich(left[key],value,path+'.'+key,changes,conflicts)


def merge_sdk_captures(captures,output,scenes=None):
    sources=[Path(p).resolve() for p in captures];output=Path(output).resolve()
    if not sources or len(set(sources))!=len(sources):raise CodegenError('Provide distinct capture directories')
    if output.exists():raise CodegenError('Merged capture output must be new')
    if any(output==p or output in p.parents or p in output.parents for p in sources):raise CodegenError('Merged output must be separate from captures')
    if scenes is not None and len(scenes)!=len(sources):raise CodegenError('Provide one scene label per capture')
    accepted={};provenance={};conflicts=[];semantic_conflicts=[];changes=[];inventories=[];primary_state=None
    sha=None;excluded=[];known_type_paths=set()
    for n,source in enumerate(sources):
        state,owners,objects=_validate_dump(source);del objects
        build=(state.get('build') or {}).get('sha256')
        if not build or (sha is not None and build!=sha):raise CodegenError('Capture module SHA-256 mismatch')
        sha=build
        replay_alignments(source,state,owners)
        replay_property_targets(source,state,owners)
        primary_state=state
        inventories.append({'capture':str(source),'scene':scenes[n] if scenes else 'unspecified','types':len(owners)})
        excluded.extend(state.get('excluded_types',[]))
        for owner in owners:
            path=owner['path']
            known_type_paths.add(path)
            if owner.get('complete') is False:
                conflicts.append({'path':path,'capture':str(source),'reason':'incomplete type rejected'});continue
            if owner['kind']!='enum' and owner.get('size') is None:continue
            previous=accepted.get(path)
            if previous is not None and layout_signature(previous)!=layout_signature(owner):
                conflicts.append({'path':path,'earlier_capture':provenance[path][-1],'capture':str(source),
                                  'reason':'layout differs; last verified scene takes precedence'})
                accepted[path]=owner
            elif previous is not None:
                combined=deepcopy(owner)
                _enrich(combined,previous,path,changes,semantic_conflicts)
                previous_fields={f['name']:f for f in previous['fields']}
                for f in combined['fields']:_enrich(f,previous_fields[f['name']],path+'.'+f['name'],changes,semantic_conflicts)
                accepted[path]=combined
            else:accepted[path]=owner
            provenance.setdefault(path,[]).append(str(source))
    # Dependencies must resolve inside the merged validated set.
    dropped=[]
    while True:
        bad=[]
        for path,owner in accepted.items():
            if owner.get('super') and owner['super'] not in accepted:bad.append(path)
            elif owner['kind']=='function' and path.rsplit('.',1)[0] in known_type_paths and path.rsplit('.',1)[0] not in accepted:bad.append(path)
        if not bad:break
        for path in bad:accepted.pop(path);dropped.append(path)
    owners=sorted(accepted.values(),key=lambda o:o['path'])
    recovered=sorted({entry.split(': ',1)[0] for entry in excluded}&accepted.keys())
    primary_paths={o['path'] for o in owners if str(sources[-1]) in provenance[o['path']]}
    missing=sorted(set(accepted)-primary_paths)
    report={'format':'EbonSdkMerge-v1','module_sha256':sha,'captures':inventories,
            'summary':{'types':len(owners),'types_added_to_primary':len(missing),'excluded_types_recovered':len(recovered),
                       'layout_conflicts':len(conflicts),'semantic_conflicts':len(semantic_conflicts),'semantic_enrichments':len(changes)},
            'types_added_to_primary':missing,'excluded_types_recovered':recovered,'layout_conflicts':conflicts,
            'semantic_conflicts':semantic_conflicts,'dependency_exclusions':dropped,'type_sources':provenance,
            'pending_scenes':[s for s in ('login','updater','character_creation') if not scenes or s not in scenes],
            'packet_mapping_verified':False}
    output.mkdir(parents=True,exist_ok=False)
    state=deepcopy(primary_state)
    state.update(types=len(owners),properties=sum(len(o['fields']) for o in owners),phase='merged',
                 progress_done=len(owners),progress_total=len(owners),sdk_validated_subset=True,
                 excluded_types=list(dict.fromkeys(e for e in excluded if e.split(': ',1)[0] not in accepted)),
                 merged_captures=inventories,coverage='union of observed verified scenes; exhaustive coverage not established')
    try:
        (output/'types.jsonl').write_text(''.join(json.dumps(o,ensure_ascii=False)+'\n' for o in owners),encoding='utf-8')
        shutil.copyfile(sources[-1]/'objects.jsonl',output/'objects.jsonl')
        (output/'dump_state.json').write_text(json.dumps(state,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
        (output/'merge_report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    except Exception:
        (output/'generation_failed.txt').write_text('Merge interrupted; output is incomplete.\n',encoding='utf-8');raise
    return report
