# Offline plan test: the AWS provider is mocked, so `tofu test` makes no AWS call and needs no credentials.
#   cd infra/terraform && tofu init -backend=false && tofu test
mock_provider "aws" {
  mock_data "aws_route53_zone" {
    defaults = {
      name_servers = ["ns-1.awsdns-00.org", "ns-2.awsdns-00.com"]
    }
  }
  mock_data "aws_ssm_parameter" {
    defaults = {
      value = "ami-0123456789abcdef0"
    }
  }
  mock_data "aws_iam_policy_document" {
    defaults = {
      json = "{\"Version\":\"2012-10-17\",\"Statement\":[]}"
    }
  }
  mock_resource "aws_ebs_volume" {
    defaults = {
      id = "vol-0123456789abcdef0"
    }
  }
  mock_resource "aws_ecr_repository" {
    defaults = {
      arn = "arn:aws:ecr:us-east-1:123456789012:repository/mock"
    }
  }
  mock_resource "aws_cloudwatch_log_group" {
    defaults = {
      arn = "arn:aws:logs:us-east-1:123456789012:log-group:/napple/servers"
    }
  }
}

run "defaults" {
  command = plan

  assert {
    condition     = aws_route53_record.server.name == "pretend.napple.lat" && aws_route53_record.server.type == "A"
    error_message = "pretend.napple.lat A record missing"
  }
  assert {
    condition     = aws_route53_record.www[0].records == toset(["erostalevi.github.io"])
    error_message = "www must CNAME to <owner>.github.io"
  }
  assert {
    condition     = length(aws_route53_record.apex_a[0].records) == 4 && length(aws_route53_record.apex_aaaa[0].records) == 4
    error_message = "apex needs the four GitHub Pages A and AAAA addresses"
  }
  assert {
    condition     = aws_route53_record.wildcard[0].name == "*.napple.lat"
    error_message = "wildcard record missing"
  }
  assert {
    condition     = length(aws_route53_record.pages_verification) == 0
    error_message = "no verification TXT without a value"
  }
  assert {
    condition     = aws_instance.servers.instance_type == "t4g.micro" && aws_instance.servers.metadata_options[0].http_tokens == "required"
    error_message = "instance type or IMDSv2 wrong"
  }
  assert {
    condition     = strcontains(aws_instance.servers.user_data, "vol=\"vol0123456789abcdef0\"")
    error_message = "user data does not find the data volume by id"
  }
  assert {
    condition     = !contains([for r in aws_vpc_security_group_ingress_rule.v4 : r.from_port], 22)
    error_message = "SSH must stay closed (SSM only)"
  }
  assert {
    condition     = !contains(keys(aws_vpc_security_group_ingress_rule.v4), "nex") && aws_vpc_security_group_ingress_rule.v4["room_tcp"].from_port == 31415
    error_message = "no NEX UDP range (NEX is WebSocket on 443); the room port must be open"
  }
  assert {
    condition = length(aws_vpc_security_group_ingress_rule.natcheck) == 16 && alltrue([
      for p in [10025, 10125, 33334, 33335, 11025, 11125, 34334, 34335] :
      aws_vpc_security_group_ingress_rule.natcheck["${p}-v4"].cidr_ipv4 == "0.0.0.0/0" && aws_vpc_security_group_ingress_rule.natcheck["${p}-v6"].cidr_ipv6 == "::/0" && aws_vpc_security_group_ingress_rule.natcheck["${p}-v4"].ip_protocol == "udp"
    ])
    error_message = "the 8 NAT-check UDP ports must be open on IPv4 and IPv6"
  }
  assert {
    condition     = startswith(aws_iam_role.instance.name, "napple-") && startswith(aws_iam_instance_profile.instance.name, "napple-")
    error_message = "IAM resources must be named napple-* (the deploy role may only manage role/napple-* and instance-profile/napple-*)"
  }
  assert {
    condition     = aws_ecr_repository.image["napple-online"].image_tag_mutability == "IMMUTABLE"
    error_message = "image tags must be immutable"
  }
}

run "cost_defaults" {
  command = plan
  assert {
    condition     = aws_instance.servers.root_block_device[0].volume_size == 8 && aws_ebs_volume.data[0].size == 1 && !aws_instance.servers.monitoring
    error_message = "volumes must stay at the minimum sizes and detailed monitoring off"
  }
  assert {
    condition     = aws_instance.servers.credit_specification[0].cpu_credits == "standard"
    error_message = "unlimited credits can bill surplus CPU"
  }
  assert {
    condition     = jsondecode(aws_ecr_lifecycle_policy.image["napple-room"].policy).rules[0].selection.countNumber == 3
    error_message = "ECR keeps the last 3 images"
  }
  assert {
    condition     = aws_cloudwatch_log_group.servers.retention_in_days == 3
    error_message = "log retention must be 3 days"
  }
  assert {
    condition     = aws_route53_record.www[0].allow_overwrite
    error_message = "the stack takes over the hand-made Pages records by default"
  }
}

run "no_data_volume" {
  command = plan
  variables {
    data_volume_gb = 0
  }
  assert {
    condition     = length(aws_ebs_volume.data) == 0 && length(aws_volume_attachment.data) == 0
    error_message = "data_volume_gb = 0 must not create a volume"
  }
  assert {
    condition     = strcontains(aws_instance.servers.user_data, "vol=\"\"") && strcontains(aws_instance.servers.user_data, "no data volume configured")
    error_message = "user data must skip the volume"
  }
}

run "pages_off_and_verification" {
  command = plan
  variables {
    wildcard_to_pages             = false
    github_pages_verification_txt = "abc123"
    github_owner                  = "SomeOrg"
  }
  assert {
    condition     = length(aws_route53_record.wildcard) == 0
    error_message = "wildcard_to_pages = false must drop the wildcard"
  }
  assert {
    condition     = aws_route53_record.pages_verification[0].name == "_github-pages-challenge-someorg.napple.lat"
    error_message = "verification TXT name wrong"
  }
  assert {
    condition     = aws_route53_record.www[0].records == toset(["someorg.github.io"])
    error_message = "owner is not lower-cased in the CNAME target"
  }
}

run "zone_by_id" {
  command = plan
  variables {
    zone_id = "Z0EXPLICITZONE"
  }
  assert {
    condition     = data.aws_route53_zone.main.zone_id == "Z0EXPLICITZONE" && aws_route53_record.server.zone_id == "Z0EXPLICITZONE"
    error_message = "NAPPLE_ZONE_ID must select the zone by id"
  }
}

run "zone_by_name" {
  command = plan
  assert {
    condition     = data.aws_route53_zone.main.name == "napple.lat"
    error_message = "without a zone id the zone is looked up by var.domain"
  }
}

run "x86_64" {
  command = plan
  variables {
    instance_type = "t3.small"
    architecture  = "x86_64"
  }
  assert {
    condition     = data.aws_ssm_parameter.al2023.name == "/aws/service/ami-amazon-linux-latest/al2023-ami-kernel-default-x86_64"
    error_message = "AMI parameter does not follow architecture"
  }
}

run "bad_architecture" {
  command = plan
  variables {
    architecture = "riscv64"
  }
  expect_failures = [var.architecture]
}
