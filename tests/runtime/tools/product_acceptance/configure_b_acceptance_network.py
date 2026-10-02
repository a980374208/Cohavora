"""Inspect/allow only task-owned SFU ports using the existing Workbench auth."""
from __future__ import annotations

import argparse
import base64
from datetime import datetime, timezone
import hashlib
import hmac
import ipaddress
import json
from pathlib import Path
import urllib.parse
import urllib.request
import uuid

INSTANCE = "i-2zefof0rekjzkbyi8c7j"
REGION = "cn-beijing"
LABEL = "codex-b-acceptance-20261001"


def credential():
    config = json.loads((Path.home() / ".workbench/config.json").read_text())
    found = []

    def visit(value):
        if isinstance(value, dict):
            fields = {str(k).lower().replace("_", "").replace("-", ""): v for k, v in value.items()}
            key = fields.get("accesskeyid")
            secret = fields.get("accesskeysecret")
            if key and secret:
                found.append((key, secret, fields.get("securitytoken")))
            for child in value.values():
                visit(child)
        elif isinstance(value, list):
            for child in value:
                visit(child)

    visit(config)
    if len(found) != 1:
        raise RuntimeError("ambiguous_cloud_auth")
    return found[0]


def api(action, **args):
    key, secret, token = credential()
    params = dict(Action=action, Version="2014-05-26", Format="JSON", RegionId=REGION,
                  AccessKeyId=key, SignatureMethod="HMAC-SHA1", SignatureVersion="1.0",
                  SignatureNonce=uuid.uuid4().hex,
                  Timestamp=datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"), **args)
    if token:
        params["SecurityToken"] = token
    quote = lambda value: urllib.parse.quote(str(value), safe="~")
    canonical = "&".join(quote(k) + "=" + quote(v) for k, v in sorted(params.items()))
    signature = hmac.new((secret + "&").encode(), ("GET&%2F&" + quote(canonical)).encode(), hashlib.sha1)
    params["Signature"] = base64.b64encode(signature.digest()).decode()
    try:
        response = urllib.request.urlopen("https://ecs.cn-beijing.aliyuncs.com/?" + urllib.parse.urlencode(params), timeout=20)
    except urllib.error.HTTPError as error:
        data = json.loads(error.read())
        raise RuntimeError("cloud_api_" + str(data.get("Code", "failed"))) from None
    data = json.load(response)
    if "Code" in data:
        raise RuntimeError("cloud_api_" + str(data["Code"]))
    return data


def permits(rule, protocol, port):
    if str(rule.get("Policy", "Accept")).lower() != "accept":
        return False
    if str(rule.get("IpProtocol", "")).lower() not in (protocol, "all"):
        return False
    # Reuse the shared SFU's existing IPv4 access scope, never invent a broader one.
    if not rule.get("SourceCidrIp"):
        return False
    start, stop = map(int, rule["PortRange"].split("/"))
    return start == -1 or start <= port <= stop


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--cleanup", action="store_true")
    parser.add_argument("--source-cidr")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.apply and args.cleanup:
        parser.error("apply and cleanup are mutually exclusive")
    source = ipaddress.ip_network(args.source_cidr) if args.source_cidr else None
    if args.apply and (source is None or source.version != 4 or source.prefixlen != 32):
        parser.error("apply requires the verified client's single IPv4 /32")
    args.output.mkdir(parents=True, exist_ok=False)
    instances = api("DescribeInstances", InstanceIds=json.dumps([INSTANCE]))["Instances"]["Instance"]
    if len(instances) != 1 or instances[0]["InstanceId"] != INSTANCE:
        raise RuntimeError("instance_identity_mismatch")
    if "123.56.225.164" not in instances[0]["PublicIpAddress"]["IpAddress"]:
        raise RuntimeError("instance_address_mismatch")
    groups = instances[0]["SecurityGroupIds"]["SecurityGroupId"]
    before = {group: api("DescribeSecurityGroupAttribute", SecurityGroupId=group)["Permissions"]["Permission"]
              for group in groups}
    added = []
    if args.cleanup:
        for group, rules in before.items():
            for rule in rules:
                if rule.get("Description") != LABEL:
                    continue
                if rule.get("PortRange") not in ("17980/17980", "17981/17981", "17982/17982"):
                    raise RuntimeError("cleanup_scope_mismatch")
                rule_id = rule.get("SecurityGroupRuleId")
                if not rule_id:
                    raise RuntimeError("cleanup_rule_id_missing")
                api("RevokeSecurityGroup", SecurityGroupId=group, **{"SecurityGroupRuleId.1": rule_id})
                added.append(dict(group=group, removed_rule=rule_id))
    for protocol, original, ports in [("tcp", 17880, (17980, 17981)), ("udp", 17882, (17982,))]:
        if args.cleanup:
            break
        rules = [(group, rule) for group, values in before.items() for rule in values]
        scopes = [(group, rule["SourceCidrIp"]) for group, rule in rules if permits(rule, protocol, original)]
        if not scopes:
            raise RuntimeError("shared_service_scope_not_found")
        group, scope = scopes[0]
        if source is not None:
            if not any(source.subnet_of(ipaddress.ip_network(allowed)) for _, allowed in scopes):
                raise RuntimeError("source_exceeds_existing_scope")
            scope = str(source)
        for port in ports:
            if any(permits(rule, protocol, port) and ipaddress.ip_network(scope).subnet_of(
                    ipaddress.ip_network(rule["SourceCidrIp"])) for _, rule in rules):
                continue
            item = dict(group=group, protocol=protocol, port=port, source_cidr=scope)
            if args.apply:
                api("AuthorizeSecurityGroup", SecurityGroupId=group, IpProtocol=protocol,
                    PortRange=f"{port}/{port}", SourceCidrIp=scope, Policy="Accept", Description=LABEL)
                item["applied"] = True
            else:
                item["applied"] = False
            added.append(item)
    keys = ("IpProtocol", "PortRange", "SourceCidrIp", "Policy", "Priority", "SecurityGroupRuleId", "Description")
    after = {group: [{k: v for k, v in rule.items() if k in keys}
                     for rule in api("DescribeSecurityGroupAttribute", SecurityGroupId=group)["Permissions"]["Permission"]]
             for group in groups}
    result = dict(instance=INSTANCE, status="CLEANED" if args.cleanup else "APPLIED" if args.apply and added else "INSPECTED",
                  added=added, rules=after)
    (args.output / "network.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(dict(status=result["status"], added=added)))


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        # Never print signed URLs, credential data or raw cloud exceptions.
        print(json.dumps(dict(status="FAIL", reason=str(error) if isinstance(error, RuntimeError) else type(error).__name__)))
        raise SystemExit(1)
